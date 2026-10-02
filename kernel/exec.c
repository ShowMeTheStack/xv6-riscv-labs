#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

// map ELF permissions to PTE permission bits.
int
flags2perm(int flags)
{
  int perm = 0;
  if (flags & 0x1)
    perm = PTE_X;
  if (flags & 0x2)
    perm |= PTE_W;
  return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip, *oldexe;
  struct proghdr ph;
  struct seg seg[NSEG];
  int nseg = 0;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  // Open the executable file.
  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if (elf.magic != ELF_MAGIC)
    goto bad;

  if ((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Record the program's segments. Their pages are read from ip
  // when the program first touches them (loadpage).
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    if (ph.vaddr < sz || nseg == NSEG) // overlapping, or too many
      goto bad;
    if (ph.vaddr + ph.memsz > TRAPFRAME - (USERSTACK + 1) * PGSIZE)
      goto bad; // no room for the guard page and the stack
    if (ph.off + ph.filesz < ph.off || ph.off + ph.filesz > ip->size)
      goto bad; // the file must hold the segment's bytes
    sz = ph.vaddr + ph.memsz;
    // remember where the segment's bytes are in the file.
    seg[nseg].va = ph.vaddr;
    seg[nseg].memsz = ph.memsz;
    seg[nseg].off = ph.off;
    seg[nseg].filesz = ph.filesz;
    seg[nseg].perm = flags2perm(ph.flags);
    nseg++;
  }
  // ip stays locked, and the transaction open, until the new
  // image is committed below.

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if ((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK + 1) * PGSIZE, PTE_W)) ==
      0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz - (USERSTACK + 1) * PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK * PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for (argc = 0; argv[argc]; argc++) {
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if (sp < stackbase)
      goto bad;
    if (copyout(pagetable, sz, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase)
    goto bad;
  if (copyout(pagetable, sz, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) <
      0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image.
  oldpagetable = p->pagetable;
  oldexe = p->exe;
  p->pagetable = pagetable;
  p->sz = sz;
  // keep namei's reference: the image's pages come from ip.
  p->exe = ip;
  memmove(p->seg, seg, sizeof(seg));
  p->nseg = nseg;
  p->trapframe->epc = elf.entry; // initial program counter = ulib.c:start()
  p->trapframe->sp = sp;         // initial stack pointer
  iunlock(ip);
  if (oldexe)
    iput(oldexe); // inside the transaction: it may free the file
  end_op();
  proc_freepagetable(oldpagetable, oldsz);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

bad:
  if (pagetable)
    proc_freepagetable(pagetable, sz);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// The end of p's program image. Addresses below it belong to the
// program's segments; the stack and the heap lie above it.
uint64
imgend(struct proc *p)
{
  struct seg *s;

  if (p->nseg == 0)
    return 0;
  s = &p->seg[p->nseg - 1];
  return s->va + s->memsz;
}

// p has shrunk to sz (sbrk with a negative size). Forget the parts
// of its segments above sz, page by page, so that memory the
// process grows into later is zeroed, as before, and is not read
// from the file.
void
segtrim(struct proc *p, uint64 sz)
{
  uint64 top = PGROUNDUP(sz);
  int i;

  for (i = 0; i < p->nseg && p->seg[i].va < top; i++) {
    struct seg *s = &p->seg[i];
    if (s->va + s->memsz > top)
      s->memsz = top - s->va;
    if (s->filesz > s->memsz)
      s->filesz = s->memsz;
  }
  p->nseg = i; // drop the segments entirely above sz
}

// Read the page at va of p's program image from the executable
// and map it with its segment's permissions. va must be
// page-aligned, below imgend(p), and not mapped. Called by
// vmfault. May sleep, so the caller must hold no spinlock (and
// not the executable's inode lock). Returns the page's physical
// address, or 0 if va is in no segment or the page can't be loaded.
uint64
loadpage(struct proc *p, uint64 va)
{
  struct seg *s;
  uint64 off;
  uint n;
  char *mem;

  for (s = p->seg; s < &p->seg[p->nseg]; s++)
    if (va >= s->va && va < s->va + s->memsz)
      break;
  if (s == &p->seg[p->nseg])
    return 0; // a hole between segments

  if ((mem = kalloc()) == 0)
    return 0;
  // zero first: what the file does not supply (the bss, the end
  // of the last page) must read as 0, not as kalloc's junk.
  memset(mem, 0, PGSIZE);
  off = va - s->va;
  if (off < s->filesz) {
    n = PGSIZE;
    if (s->filesz - off < PGSIZE)
      n = s->filesz - off;
    ilock(p->exe);
    if (readi(p->exe, 0, (uint64)mem, s->off + off, n) != n) {
      iunlock(p->exe); // the file is shorter than the program
      kfree(mem);
      return 0;
    }
    iunlock(p->exe);
  }
  if (mappages(p->pagetable, va, PGSIZE, (uint64)mem,
               PTE_R | PTE_U | s->perm) != 0) {
    kfree(mem);
    return 0;
  }
  return (uint64)mem;
}
