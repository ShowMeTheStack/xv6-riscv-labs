        #
        # copies between kernel memory and user memory that use
        # the user's virtual addresses directly. they set
        # sstatus.SUM, which lets supervisor-mode loads and stores
        # use PTE_U pages, for the copy loop only. the page table
        # in satp must map the user's pages: p->kpagetable.
        #
        # kerneltrap() sends a page fault taken on any instruction
        # from ucopy up to ucopyend to ucopyfail.
        #

.equ SSTATUS_SUM, 1 << 18

.globl ucopy
.globl ucopystr
.globl ucopyend
.globl ucopyfail

        # int ucopy(char *dst, char *src, uint64 n)
        # copy n bytes from src to dst; return 0.
ucopy:
        li t0, SSTATUS_SUM
        csrs sstatus, t0
1:
        beqz a2, 2f
        lbu t1, 0(a1)
        sb t1, 0(a0)
        addi a0, a0, 1
        addi a1, a1, 1
        addi a2, a2, -1
        j 1b
2:
        csrc sstatus, t0
        li a0, 0
        ret

        # int ucopystr(char *dst, char *src, uint64 max)
        # copy bytes up to and including a '\0', at most max.
        # return 0 if a '\0' was copied, -1 if not.
ucopystr:
        li t0, SSTATUS_SUM
        csrs sstatus, t0
1:
        beqz a2, 3f
        lbu t1, 0(a1)
        sb t1, 0(a0)
        beqz t1, 2f
        addi a0, a0, 1
        addi a1, a1, 1
        addi a2, a2, -1
        j 1b
2:
        csrc sstatus, t0
        li a0, 0
        ret
3:
        csrc sstatus, t0
        li a0, -1
        ret
ucopyend:

        # a copy that faulted on a bad user address resumes
        # here: turn SUM off and return -1 to the caller.
ucopyfail:
        li t0, SSTATUS_SUM
        csrc sstatus, t0
        li a0, -1
        ret
