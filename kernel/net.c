//
// the network stack: just enough Ethernet, ARP, IPv4 and UDP to
// talk to QEMU's user-mode network ("slirp").
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "net.h"

// xv6's address on slirp's network.
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// the MAC address with which slirp answers ARP for the host
// (10.0.2.2). slirp takes IP packets for the host, its DNS
// server (10.0.2.3) and the rest of the world without checking
// their destination MAC, so xv6 sends every packet here and
// needs no ARP requests of its own.
static uint8 host_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

// the Internet checksum (RFC 1071) of len bytes at p,
// in network byte order.
static uint16
in_cksum(uint8 *p, int len)
{
  uint32 sum = 0;

  for (; len > 1; len -= 2, p += 2)
    sum += (p[0] << 8) | p[1];
  if (len == 1)
    sum += p[0] << 8;
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return htons(~sum);
}

// send a UDP packet to dip:dport from local port sport. buf is a
// kalloc() page holding len bytes of data at offset NET_HDRS;
// this fills in the headers in front of the data. buf belongs to
// the driver afterwards, or is freed on failure.
int
net_tx_udp(char *buf, int len, uint32 dip, uint16 dport, uint16 sport)
{
  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);
  struct udp *udp = (struct udp *)(ip + 1);

  memmove(eth->dhost, host_mac, 6);
  memmove(eth->shost, local_mac, 6);
  eth->type = htons(ETHTYPE_IP);

  memset(ip, 0, sizeof(*ip));
  ip->ip_vhl = 0x45;
  ip->ip_len = htons(sizeof(*ip) + sizeof(*udp) + len);
  ip->ip_ttl = 64;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dip);
  ip->ip_sum = in_cksum((uint8 *)ip, sizeof(*ip));

  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(sizeof(*udp) + len);
  udp->sum = 0;

  if (e1000_transmit(buf, NET_HDRS + len) < 0) {
    kfree(buf);
    return -1;
  }
  return 0;
}

// answer an ARP request for local_ip, turning the request's
// buffer into the reply.
static void
arp_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *)buf;
  struct arp *arp = (struct arp *)(eth + 1);

  if (len < sizeof(*eth) + sizeof(*arp) || ntohs(arp->hrd) != ARP_HRD_ETHER ||
      ntohs(arp->pro) != ETHTYPE_IP || arp->hln != 6 || arp->pln != 4 ||
      ntohs(arp->op) != ARP_OP_REQUEST || ntohl(arp->tip) != local_ip) {
    kfree(buf);
    return;
  }

  arp->op = htons(ARP_OP_REPLY);
  memmove(arp->tha, arp->sha, 6);
  arp->tip = arp->sip;
  memmove(arp->sha, local_mac, 6);
  arp->sip = htonl(local_ip);

  memmove(eth->dhost, eth->shost, 6);
  memmove(eth->shost, local_mac, 6);

  if (e1000_transmit(buf, sizeof(*eth) + sizeof(*arp)) < 0)
    kfree(buf);
}

// a UDP packet with len bytes of data at buf + NET_HDRS has
// arrived for local port dport. for now, drop it.
static void
udp_rx(char *buf, int len, uint32 sip, uint16 sport, uint16 dport)
{
  kfree(buf);
}

// check an IPv4 packet's headers and pass UDP packets on.
static void
ip_rx(char *buf, int len)
{
  struct ip *ip = (struct ip *)(buf + sizeof(struct eth));
  struct udp *udp = (struct udp *)(ip + 1);

  // xv6 understands only plain UDP packets for itself: no IP
  // options, no fragments.
  if (len < NET_HDRS || ip->ip_vhl != 0x45 || ip->ip_p != IPPROTO_UDP ||
      ntohl(ip->ip_dst) != local_ip || (ntohs(ip->ip_off) & 0x3fff) != 0 ||
      in_cksum((uint8 *)ip, sizeof(*ip)) != 0) {
    kfree(buf);
    return;
  }

  // the lengths in the headers must fit in what arrived.
  int iplen = ntohs(ip->ip_len);
  int ulen = ntohs(udp->ulen);
  if (iplen > len - (int)sizeof(struct eth) || ulen < (int)sizeof(*udp) ||
      ulen > iplen - (int)sizeof(*ip)) {
    kfree(buf);
    return;
  }

  udp_rx(buf, ulen - sizeof(*udp), ntohl(ip->ip_src), ntohs(udp->sport),
         ntohs(udp->dport));
}

// a packet of len bytes has arrived in buf (a kalloc() page).
// called from e1000_intr() with interrupts off: must not sleep.
// every path ends by freeing buf or passing it on.
void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *)buf;

  if (len < sizeof(*eth)) {
    kfree(buf);
    return;
  }
  if (ntohs(eth->type) == ETHTYPE_ARP)
    arp_rx(buf, len);
  else if (ntohs(eth->type) == ETHTYPE_IP)
    ip_rx(buf, len);
  else
    kfree(buf);
}
