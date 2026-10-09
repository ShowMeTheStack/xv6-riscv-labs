//
// just enough PCI to find qemu's e1000 network card.
//
// qemu -machine virt has a PCIe host bridge whose configuration
// space is memory-mapped ("ECAM"): every bus/device/function has a
// 4096-byte block of registers at
//   PCIE_ECAM + (bus << 20) + (device << 15) + (function << 12).
// xv6 looks only at bus 0, function 0 of each device.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// 32-bit words of a configuration header.
#define PCI_ID      0  // vendor ID (low 16 bits), device ID (high 16)
#define PCI_COMMAND 1  // command (low 16 bits), status (high 16)
#define PCI_BAR0    4  // base address register 0
#define PCI_INTR    15 // interrupt line (bits 0-7), pin (bits 8-15)

#define PCI_COMMAND_MEMORY 0x2 // respond to accesses inside the BARs
#define PCI_COMMAND_MASTER 0x4 // allow the device to do DMA

// device 0x100e (82540EM) from vendor 0x8086 (Intel).
#define E1000_ID 0x100e8086

void
pci_init(void)
{
  for (int dev = 0; dev < 32; dev++) {
    volatile uint32 *h = (volatile uint32 *)(PCIE_ECAM + (dev << 15));
    if (h[PCI_ID] != E1000_ID)
      continue;

    // how many bytes of registers does BAR 0 decode? write all
    // ones: the device keeps the address bits below its size zero.
    h[PCI_BAR0] = 0xffffffff;
    uint32 size = ~(h[PCI_BAR0] & ~0xf) + 1;
    if (size > E1000_REGS_SIZE)
      panic("pci: e1000 registers too big");

    // place the registers at E1000_REGS, then let the device
    // decode them and do DMA.
    h[PCI_BAR0] = E1000_REGS;
    h[PCI_COMMAND] = PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;

    int pin = (h[PCI_INTR] >> 8) & 0xff; // 1 = INTA ... 4 = INTD
    printk("pci: e1000 at 0:%d.0, %d bytes of registers at 0x%lx, INT%c\n",
           dev, size, E1000_REGS, 'A' + pin - 1);
    e1000_init();
    return;
  }
  panic("pci: no e1000");
}
