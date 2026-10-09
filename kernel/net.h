//
// packet formats for Ethernet, ARP, IPv4 and UDP.
// all multi-byte fields are big-endian ("network byte order").
//

// RISC-V is little-endian: swap the bytes of a 16- or 32-bit
// value between host and network byte order.
static inline uint16
htons(uint16 x)
{
  return (x << 8) | (x >> 8);
}

static inline uint32
htonl(uint32 x)
{
  return ((x & 0xff) << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) |
         (x >> 24);
}

#define ntohs htons
#define ntohl htonl

#define MAKE_IP_ADDR(a, b, c, d)                                               \
  (((uint32)(a) << 24) | ((uint32)(b) << 16) | ((uint32)(c) << 8) | (uint32)(d))

// the headers are packed: inside a packet, a 32-bit field may
// sit at an address that is not a multiple of 4.

struct eth {
  uint8 dhost[6];
  uint8 shost[6];
  uint16 type;
} __attribute__((packed));

#define ETHTYPE_IP  0x0800
#define ETHTYPE_ARP 0x0806

struct arp {
  uint16 hrd; // hardware type: 1 = Ethernet
  uint16 pro; // protocol type: ETHTYPE_IP
  uint8 hln;  // hardware address length: 6
  uint8 pln;  // protocol address length: 4
  uint16 op;
  uint8 sha[6]; // sender's MAC address
  uint32 sip;   // sender's IP address
  uint8 tha[6]; // target's MAC address
  uint32 tip;   // target's IP address
} __attribute__((packed));

#define ARP_HRD_ETHER 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct ip {
  uint8 ip_vhl; // version (4) and header length in words (5)
  uint8 ip_tos;
  uint16 ip_len; // header and data, in bytes
  uint16 ip_id;
  uint16 ip_off; // fragment flags and offset
  uint8 ip_ttl;
  uint8 ip_p; // protocol: IPPROTO_UDP
  uint16 ip_sum;
  uint32 ip_src;
  uint32 ip_dst;
} __attribute__((packed));

#define IPPROTO_UDP 17

struct udp {
  uint16 sport;
  uint16 dport;
  uint16 ulen; // header and data, in bytes
  uint16 sum;  // 0: no checksum
} __attribute__((packed));

// a UDP packet's data starts this many bytes into the frame.
#define NET_HDRS (sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp))

// the most data one UDP packet may carry (1500-byte IP packets).
#define NET_MAXDATA (1500 - sizeof(struct ip) - sizeof(struct udp))
