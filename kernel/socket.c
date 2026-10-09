//
// UDP sockets: bind a local port, then send and receive
// datagrams through it.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "net.h"

#define NSOCK 16 // sockets in the whole system
#define NQ    16 // packets queued on one socket, at most

struct sock {
  struct spinlock lock;
  int port; // the bound local port, or 0 if free. changes only
            // with both socktab.lock and this lock held.

  // packets that arrived and wait for recv(): q[head] is the
  // oldest, n of them in all.
  int head;
  int n;
  struct {
    char *buf; // the packet's kalloc() page; data at NET_HDRS
    int len;
    uint32 sip;
    uint16 sport;
  } q[NQ];
};

struct {
  struct spinlock lock; // protects the choice of each sock's port
  struct sock sock[NSOCK];
} socktab;

void
sockinit(void)
{
  initlock(&socktab.lock, "socktab");
  for (int i = 0; i < NSOCK; i++)
    initlock(&socktab.sock[i].lock, "sock");
}

// find the socket bound to port and return it locked, or 0.
// the caller must hold socktab.lock.
static struct sock *
sockfind(int port)
{
  for (int i = 0; i < NSOCK; i++) {
    struct sock *s = &socktab.sock[i];
    if (s->port == port) {
      acquire(&s->lock);
      return s;
    }
  }
  return 0;
}

// called from ip_rx(), inside the e1000 interrupt: queue the
// packet on the socket bound to dport, or drop it. must not sleep.
void
sock_rx(char *buf, int len, uint32 sip, uint16 sport, uint16 dport)
{
  // port 0 marks a free socket; no packet may go there.
  if (dport == 0) {
    kfree(buf);
    return;
  }

  acquire(&socktab.lock);
  struct sock *s = sockfind(dport);
  release(&socktab.lock);
  if (s == 0) {
    kfree(buf);
    return;
  }

  if (s->n == NQ) {
    // nobody is reading fast enough: drop the packet.
    release(&s->lock);
    kfree(buf);
    return;
  }
  int i = (s->head + s->n) % NQ;
  s->q[i].buf = buf;
  s->q[i].len = len;
  s->q[i].sip = sip;
  s->q[i].sport = sport;
  s->n++;
  wakeup(s);
  release(&s->lock);
}

// int bind(int port)
uint64
sys_bind(void)
{
  int port;

  argint(0, &port);
  if (port <= 0 || port > 0xffff)
    return -1;

  acquire(&socktab.lock);
  struct sock *s = sockfind(port);
  if (s) {
    // already bound.
    release(&s->lock);
    release(&socktab.lock);
    return -1;
  }
  s = sockfind(0);
  if (s == 0) {
    release(&socktab.lock);
    return -1;
  }
  s->port = port;
  s->head = 0;
  s->n = 0;
  release(&s->lock);
  release(&socktab.lock);
  return 0;
}

// int unbind(int port)
uint64
sys_unbind(void)
{
  int port;

  argint(0, &port);
  if (port <= 0 || port > 0xffff)
    return -1;

  acquire(&socktab.lock);
  struct sock *s = sockfind(port);
  if (s == 0) {
    release(&socktab.lock);
    return -1;
  }
  s->port = 0;
  release(&socktab.lock);

  for (; s->n > 0; s->n--) {
    kfree(s->q[s->head].buf);
    s->head = (s->head + 1) % NQ;
  }
  // a process sleeping in recv() on this port must give up.
  wakeup(s);
  release(&s->lock);
  return 0;
}

// int send(uint32 dst, int dport, int sport, char *buf, int len)
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int dst, dport, sport, len;
  uint64 addr;

  argint(0, &dst);
  argint(1, &dport);
  argint(2, &sport);
  argaddr(3, &addr);
  argint(4, &len);
  if (len < 0 || len > NET_MAXDATA || dport <= 0 || dport > 0xffff ||
      sport <= 0 || sport > 0xffff)
    return -1;

  char *buf = kalloc();
  if (buf == 0)
    return -1;
  if (copyin(p->pagetable, p->sz, buf + NET_HDRS, addr, len) < 0) {
    kfree(buf);
    return -1;
  }
  return net_tx_udp(buf, len, dst, dport, sport);
}

// int recv(int port, uint32 *src, uint16 *sport, char *buf, int len)
// wait for a packet on a bound port. returns the number of data
// bytes copied to buf (the rest of a longer packet is lost).
uint64
sys_recv(void)
{
  struct proc *p = myproc();
  int port, maxlen;
  uint64 srcaddr, sportaddr, addr;

  argint(0, &port);
  argaddr(1, &srcaddr);
  argaddr(2, &sportaddr);
  argaddr(3, &addr);
  argint(4, &maxlen);
  if (port <= 0 || port > 0xffff || maxlen < 0)
    return -1;

  acquire(&socktab.lock);
  struct sock *s = sockfind(port);
  release(&socktab.lock);
  if (s == 0)
    return -1;

  // sleep until a packet arrives. sock_rx() queues it and calls
  // wakeup() while holding s->lock, so registering on the channel
  // before releasing s->lock means the wakeup cannot be missed.
  // give up if the port was unbound (and maybe bound again to
  // another port) or the process was killed meanwhile.
  while (1) {
    if (s->port != port || killed(p)) {
      release(&s->lock);
      return -1;
    }
    if (s->n > 0)
      break;
    sleep_prepare(s);
    release(&s->lock);
    sleep();
    acquire(&s->lock);
  }

  // take the oldest packet off the queue.
  char *buf = s->q[s->head].buf;
  int len = s->q[s->head].len;
  uint32 sip = s->q[s->head].sip;
  uint16 sport = s->q[s->head].sport;
  s->head = (s->head + 1) % NQ;
  s->n--;
  release(&s->lock);

  // copy out without holding a spinlock.
  if (len > maxlen)
    len = maxlen;
  int r = len;
  if (copyout(p->pagetable, p->sz, srcaddr, (char *)&sip, sizeof(sip)) < 0 ||
      copyout(p->pagetable, p->sz, sportaddr, (char *)&sport,
              sizeof(sport)) < 0 ||
      copyout(p->pagetable, p->sz, addr, buf + NET_HDRS, len) < 0)
    r = -1;
  kfree(buf);
  return r;
}
