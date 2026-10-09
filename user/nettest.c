//
// test the e1000 driver, the network stack and UDP sockets
// against nettest.py on the host. see the comment there.
//
//   host: python3 nettest.py echo PORT   then xv6: nettest PORT
//   xv6:  nettest server N   then host: python3 nettest.py ping FWDPORT N
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define HOST MAKE_IP(10, 0, 2, 2) // the host, as slirp shows it to xv6
#define DNS  MAKE_IP(10, 0, 2, 3) // slirp's DNS server
#define MAKE_IP(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))

int hostport;

// the bytes of round seq of process id.
void
fill(char *buf, int id, int seq, int len)
{
  for (int i = 0; i < len; i++)
    buf[i] = 'a' + (id * 7 + seq * 3 + i) % 26;
}

// one round trip through the host's echo server from local
// port lport. returns 0 if the echo is exactly what was sent.
int
roundtrip(int lport, int id, int seq, int len)
{
  static char out[1500], in[1500];
  uint32 src;
  uint16 sport;

  fill(out, id, seq, len);
  if (send(HOST, hostport, lport, out, len) < 0) {
    printf("nettest: send failed\n");
    return -1;
  }
  int n = recv(lport, &src, &sport, in, sizeof(in));
  if (n != len || src != HOST || sport != hostport || memcmp(in, out, len)) {
    printf("nettest: process %d round %d: bad echo (%d bytes, from 0x%x:%d)\n",
           id, seq, n, src, sport);
    return -1;
  }
  return 0;
}

// n round trips from lport with lengths from 1 to 1472 bytes.
int
roundtrips(int lport, int id, int n)
{
  unbind(lport); // in case a failed earlier run left it bound
  if (bind(lport) < 0) {
    printf("nettest: bind %d failed\n", lport);
    return -1;
  }
  for (int seq = 0; seq < n; seq++) {
    if (roundtrip(lport, id, seq, 1 + (seq * 97 + id * 31) % 1472) < 0)
      return -1;
  }
  unbind(lport);
  return 0;
}

// run f(arg) in a child; kill it after timeout ticks.
// returns the child's exit status, or -1 if it was killed.
int
run(int (*f)(int), int arg, int timeout)
{
  int pid = fork();
  if (pid == 0)
    exit(f(arg) == 0 ? 0 : 1);
  int dog = fork();
  if (dog == 0) {
    pause(timeout);
    kill(pid);
    exit(0);
  }
  // wait for both; the order in which they exit varies.
  int xs = -1, st, w;
  while ((w = wait(&st)) >= 0) {
    if (w == pid) {
      xs = st;
      kill(dog);
    }
  }
  return xs;
}

int
one(int arg)
{
  return roundtrips(2001, 0, 1);
}

int
many(int arg)
{
  return roundtrips(2002, 0, 100);
}

int
pingpong(int n)
{
  unbind(2003);
  if (bind(2003) < 0)
    return -1;
  for (int seq = 0; seq < n; seq++) {
    if (roundtrip(2003, 0, seq, 64) < 0)
      return -1;
  }
  unbind(2003);
  return 0;
}

// three processes, one per hart, each with its own port.
int
parallel(int npkt)
{
  for (int i = 0; i < 3; i++) {
    if (fork() == 0)
      exit(roundtrips(2010 + i, i + 1, npkt) == 0 ? 0 : 1);
  }
  int ok = 1;
  for (int i = 0; i < 3; i++) {
    int xs;
    wait(&xs);
    if (xs != 0)
      ok = 0;
  }
  return ok ? 0 : -1;
}

// recv() on a port nobody sends to sleeps; kill() must end it.
int
killrecv(int arg)
{
  char buf[16];
  uint32 src;
  uint16 sport;

  unbind(2020);
  if (bind(2020) < 0)
    return -1;
  int pid = fork();
  if (pid == 0) {
    recv(2020, &src, &sport, buf, sizeof(buf));
    exit(0); // not reached if recv() returns because of the kill
  }
  pause(5);
  kill(pid);
  int xs;
  wait(&xs);
  unbind(2020);
  return xs == -1 ? 0 : -1;
}

// ask slirp's DNS server for pdos.csail.mit.edu's address.
int
dns(int arg)
{
  static char q[64], buf[512];
  uchar *r = (uchar *)buf;
  char *name = "\4pdos\5csail\3mit\3edu";
  uint32 src;
  uint16 sport;

  // header: id 0x1234, recursion desired, one question.
  memset(q, 0, sizeof(q));
  q[0] = 0x12;
  q[1] = 0x34;
  q[2] = 0x01;
  q[5] = 1;
  int n = 12;
  strcpy(q + n, name);
  n += strlen(name) + 1;
  q[n + 1] = 1; // type A
  q[n + 3] = 1; // class IN
  n += 4;

  unbind(2030);
  if (bind(2030) < 0)
    return -1;
  if (send(DNS, 53, 2030, q, n) < 0)
    return -1;
  int len = recv(2030, &src, &sport, buf, sizeof(buf));
  unbind(2030);
  if (len < 12 || src != DNS || sport != 53 || r[0] != 0x12 || r[1] != 0x34 ||
      (r[3] & 0xf) != 0) {
    printf("nettest: dns: bad reply (%d bytes)\n", len);
    return -1;
  }

  // skip the question (the same name, type and class), then look
  // for an A record among the answers.
  int nans = (r[6] << 8) | r[7];
  int i = n;
  for (int a = 0; a < nans && i + 12 <= len; a++) {
    if ((r[i] & 0xc0) == 0xc0) {
      i += 2; // the name, compressed
    } else {
      while (i < len && r[i] != 0)
        i += r[i] + 1;
      i += 1;
    }
    int type = (r[i] << 8) | r[i + 1];
    int rdlen = (r[i + 8] << 8) | r[i + 9];
    i += 10;
    if (type == 1 && rdlen == 4 && i + 4 <= len) {
      printf("nettest: dns: pdos.csail.mit.edu is %d.%d.%d.%d\n", r[i],
             r[i + 1], r[i + 2], r[i + 3]);
      return 0;
    }
    i += rdlen;
  }
  printf("nettest: dns: no A record in the reply\n");
  return -1;
}

void
check(char *what, int xs, int *ok)
{
  if (xs == 0) {
    printf("nettest: %s: OK\n", what);
  } else {
    printf("nettest: %s: FAIL%s\n", what, xs == -1 ? " (timed out)" : "");
    *ok = 0;
  }
}

// echo n packets that arrive at port 2000 back to their sender.
void
server(int n)
{
  static char buf[1500];
  uint32 src;
  uint16 sport;

  unbind(2000);
  if (bind(2000) < 0) {
    printf("nettest server: bind 2000 failed\n");
    exit(1);
  }
  printf("nettest server: echoing %d packets on port 2000\n", n);
  for (int i = 0; i < n; i++) {
    int len = recv(2000, &src, &sport, buf, sizeof(buf));
    if (len < 0 || send(src, sport, 2000, buf, len) < 0) {
      printf("nettest server: failed\n");
      exit(1);
    }
  }
  unbind(2000);
  printf("nettest server: done\n");
  exit(0);
}

int
main(int argc, char *argv[])
{
  int ok = 1;

  if (argc == 3 && strcmp(argv[1], "server") == 0)
    server(atoi(argv[2]));
  if (argc != 2) {
    printf("usage: nettest hostport | nettest server n\n");
    exit(1);
  }
  hostport = atoi(argv[1]);

  check("one round trip", run(one, 0, 50), &ok);
  check("100 round trips", run(many, 0, 100), &ok);
  check("3 processes x 300 round trips", run(parallel, 300, 600), &ok);
  check("kill wakes recv", run(killrecv, 0, 50), &ok);
  int dnsok = 1;
  check("dns query to 10.0.2.3", run(dns, 0, 50), &dnsok);

  // latency, in clock ticks (about 0.1 s each).
  int t0 = uptime();
  int xs = run(pingpong, 1000, 600);
  int t1 = uptime();
  if (xs == 0)
    printf("nettest: 1000 round trips of 64 bytes took %d ticks\n", t1 - t0);
  else
    check("1000 round trips of 64 bytes", xs, &ok);

  if (ok && dnsok)
    printf("nettest: ALL OK\n");
  else if (ok)
    printf("nettest: ALL OK except dns\n");
  else
    printf("nettest: SOME TESTS FAILED\n");
  exit(ok ? 0 : 1);
}
