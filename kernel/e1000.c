//
// driver for qemu's e1000 network card (Intel 82540EM).
//
// qemu ... -netdev user,id=net0 -device e1000,netdev=net0,bus=pcie.0
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "e1000_dev.h"

// the address of e1000 register r (a byte offset).
#define R(r) ((volatile uint32 *)(E1000_REGS + (r)))

#define TX_RING_SIZE 16
#define RX_RING_SIZE 16

static struct e1000 {
  // the rings of descriptors the device reads by DMA. each lives
  // in a page from kalloc(); the kernel maps RAM one-to-one, so
  // the address of a descriptor is also its physical address.
  struct tx_desc *tx;
  struct rx_desc *rx;

  // the packet buffer (a kalloc() page) in each slot.
  char *tx_bufs[TX_RING_SIZE];
  char *rx_bufs[RX_RING_SIZE];

  struct spinlock lock;
} e1000;

// QEMU's default MAC address for a network card.
uint8 local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

// called by pci_init() once the registers are mapped at E1000_REGS.
void
e1000_init(void)
{
  int i;

  initlock(&e1000.lock, "e1000");

  // reset the device, with its interrupts masked.
  *R(E1000_IMC) = 0xffffffff;
  *R(E1000_CTL) |= E1000_CTL_RST;
  *R(E1000_IMC) = 0xffffffff;
  io_fence();

  // transmit ring: every slot starts out "done", with no packet.
  e1000.tx = (struct tx_desc *)kalloc();
  if (e1000.tx == 0)
    panic("e1000_init: kalloc");
  memset(e1000.tx, 0, PGSIZE);
  for (i = 0; i < TX_RING_SIZE; i++) {
    e1000.tx[i].status = E1000_TXD_STAT_DD;
    e1000.tx_bufs[i] = 0;
  }
  *R(E1000_TDBAL) = (uint64)e1000.tx;
  *R(E1000_TDBAH) = (uint64)e1000.tx >> 32;
  *R(E1000_TDLEN) = TX_RING_SIZE * sizeof(struct tx_desc);
  *R(E1000_TDH) = 0;
  *R(E1000_TDT) = 0;

  // receive ring: every slot gets an empty buffer.
  e1000.rx = (struct rx_desc *)kalloc();
  if (e1000.rx == 0)
    panic("e1000_init: kalloc");
  memset(e1000.rx, 0, PGSIZE);
  for (i = 0; i < RX_RING_SIZE; i++) {
    e1000.rx_bufs[i] = kalloc();
    if (e1000.rx_bufs[i] == 0)
      panic("e1000_init: kalloc");
    e1000.rx[i].addr = (uint64)e1000.rx_bufs[i];
  }
  *R(E1000_RDBAL) = (uint64)e1000.rx;
  *R(E1000_RDBAH) = (uint64)e1000.rx >> 32;
  *R(E1000_RDLEN) = RX_RING_SIZE * sizeof(struct rx_desc);
  // the device may fill the slots from RDH up to, but not
  // including, RDT.
  *R(E1000_RDH) = 0;
  *R(E1000_RDT) = RX_RING_SIZE - 1;

  // accept packets for our MAC address (and broadcasts, below).
  *R(E1000_RA) = local_mac[0] | (local_mac[1] << 8) | (local_mac[2] << 16) |
                 (local_mac[3] << 24);
  *R(E1000_RA + 4) = local_mac[4] | (local_mac[5] << 8) | E1000_RA_AV;
  for (i = 0; i < 128; i++)
    *R(E1000_MTA + 4 * i) = 0;

  // turn on transmit and receive.
  *R(E1000_TCTL) = E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT |
                   E1000_TCTL_COLD;
  *R(E1000_TIPG) = 10 | (8 << 10) | (6 << 20);
  *R(E1000_RCTL) = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC;

  // interrupt as soon as a packet has arrived (no delay).
  *R(E1000_RDTR) = 0;
  *R(E1000_IMS) = E1000_ICR_RXT0;
}

// hand the packet in buf (a kalloc() page) to the device. on
// success the driver owns buf and frees it once the device has
// sent it. returns -1 if the ring is full; the caller then still
// owns buf.
// may be called on any hart, from system calls and interrupts.
int
e1000_transmit(char *buf, int len)
{
  acquire(&e1000.lock);

  // TDT is the next slot the driver may fill. the device sends
  // the slots from TDH up to, but not including, TDT, so TDT
  // equal to TDH means "nothing to send": TDT must never move
  // onto TDH, and one slot always stays empty. the slot must
  // also be done (DD): the device may still be reading it.
  int i = *R(E1000_TDT);
  if ((i + 1) % TX_RING_SIZE == *R(E1000_TDH) ||
      (e1000.tx[i].status & E1000_TXD_STAT_DD) == 0) {
    release(&e1000.lock);
    return -1;
  }
  // order our reads of the slot after the device's write of DD.
  io_fence();

  // the device has sent the slot's previous packet: free it.
  if (e1000.tx_bufs[i])
    kfree(e1000.tx_bufs[i]);
  e1000.tx_bufs[i] = buf;

  e1000.tx[i].addr = (uint64)buf;
  e1000.tx[i].length = len;
  e1000.tx[i].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  e1000.tx[i].status = 0;

  // the descriptor must be in memory before the device can see
  // the new tail and read it.
  io_fence();

  // give the slot to the device.
  *R(E1000_TDT) = (i + 1) % TX_RING_SIZE;

  release(&e1000.lock);
  return 0;
}

// the e1000 raised an interrupt: some packets have arrived.
// called from devintr() with interrupts off, on whichever hart
// claimed the interrupt.
void
e1000_intr(void)
{
  char *bufs[RX_RING_SIZE];
  int lens[RX_RING_SIZE];
  int n = 0;

  acquire(&e1000.lock);

  // reading the interrupt cause clears it. a packet that arrives
  // after this read raises a new interrupt.
  *R(E1000_ICR);

  // the slot after the tail is the oldest one the device may have
  // filled. take filled slots until there are none, but go at most
  // once around the ring.
  for (int k = 0; k < RX_RING_SIZE; k++) {
    int i = (*R(E1000_RDT) + 1) % RX_RING_SIZE;
    if ((e1000.rx[i].status & E1000_RXD_STAT_DD) == 0)
      break;

    // order our reads of the descriptor and the packet after
    // the device's write of DD.
    io_fence();

    // pass the full buffer up and put a fresh one in the slot.
    // if there is no memory, drop the packet and reuse the buffer.
    char *fresh = kalloc();
    if (fresh) {
      bufs[n] = e1000.rx_bufs[i];
      lens[n] = e1000.rx[i].length;
      n++;
      e1000.rx_bufs[i] = fresh;
      e1000.rx[i].addr = (uint64)fresh;
    }
    e1000.rx[i].status = 0;

    // the descriptor must be in memory before the device can see
    // the new tail and fill the slot again.
    io_fence();

    // give the slot back to the device.
    *R(E1000_RDT) = i;
  }

  release(&e1000.lock);

  // hand the packets to the network stack without holding
  // e1000.lock: answering a packet calls e1000_transmit().
  for (int j = 0; j < n; j++)
    net_rx(bufs[j], lens[j]);
}
