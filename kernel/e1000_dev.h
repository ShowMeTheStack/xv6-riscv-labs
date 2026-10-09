//
// the parts of the Intel 8254x ("e1000") interface that xv6 uses.
// see Intel's "PCI/PCI-X Family of Gigabit Ethernet Controllers
// Software Developer's Manual" (document 317453006EN).
//

// registers: byte offsets from the start of BAR 0.
#define E1000_CTL   0x00000 // device control
#define E1000_ICR   0x000C0 // interrupt cause read (reading clears it)
#define E1000_IMS   0x000D0 // interrupt mask set
#define E1000_IMC   0x000D8 // interrupt mask clear
#define E1000_RCTL  0x00100 // receive control
#define E1000_TCTL  0x00400 // transmit control
#define E1000_TIPG  0x00410 // transmit inter-packet gap
#define E1000_RDBAL 0x02800 // receive ring address, low 32 bits
#define E1000_RDBAH 0x02804 // receive ring address, high 32 bits
#define E1000_RDLEN 0x02808 // receive ring length in bytes
#define E1000_RDH   0x02810 // receive head (the device's next slot)
#define E1000_RDT   0x02818 // receive tail (the driver's next slot)
#define E1000_RDTR  0x02820 // receive interrupt delay
#define E1000_TDBAL 0x03800 // transmit ring address, low 32 bits
#define E1000_TDBAH 0x03804 // transmit ring address, high 32 bits
#define E1000_TDLEN 0x03808 // transmit ring length in bytes
#define E1000_TDH   0x03810 // transmit head (the device's next slot)
#define E1000_TDT   0x03818 // transmit tail (the driver's next slot)
#define E1000_MTA   0x05200 // multicast table, 128 words
#define E1000_RA    0x05400 // receive address (our MAC), 2 words

#define E1000_CTL_RST 0x04000000 // reset the device

#define E1000_TCTL_EN   0x00000002 // enable transmit
#define E1000_TCTL_PSP  0x00000008 // pad short packets
#define E1000_TCTL_CT   0x00000100 // collision threshold 16
#define E1000_TCTL_COLD 0x00040000 // collision distance 64 (full duplex)

#define E1000_RCTL_EN    0x00000002 // enable receive
#define E1000_RCTL_BAM   0x00008000 // accept broadcast packets
#define E1000_RCTL_SECRC 0x04000000 // strip the Ethernet CRC
// buffer size bits 0 in RCTL: 2048-byte receive buffers.

#define E1000_RA_AV 0x80000000 // receive address valid

#define E1000_ICR_RXT0 0x00000080 // receiver timer interrupt

// transmit descriptor (legacy format, section 3.3.3).
struct tx_desc {
  uint64 addr; // physical address of the packet
  uint16 length;
  uint8 cso;
  uint8 cmd;
  uint8 status;
  uint8 css;
  uint16 special;
};

#define E1000_TXD_CMD_EOP  0x01 // end of packet
#define E1000_TXD_CMD_RS   0x08 // report status: set DD when done
#define E1000_TXD_STAT_DD  0x01 // descriptor done

// receive descriptor (section 3.2.3).
struct rx_desc {
  uint64 addr; // physical address of a 2048-byte buffer
  uint16 length;
  uint16 csum;
  uint8 status;
  uint8 errors;
  uint16 special;
};

#define E1000_RXD_STAT_DD  0x01 // descriptor done
#define E1000_RXD_STAT_EOP 0x02 // end of packet
