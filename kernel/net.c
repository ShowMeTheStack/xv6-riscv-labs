//
// the network stack.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"

// a packet of len bytes has arrived in buf (a kalloc() page).
// called from e1000_intr() with interrupts off; must not sleep.
// for now, drop it.
void
net_rx(char *buf, int len)
{
  kfree(buf);
}
