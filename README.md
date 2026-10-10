# xv6-riscv-labs

**Reference solutions for the 25 extension labs of
[xv6, line by line](https://xv6.jyotiprakash.org/labs/)**, by Jyotiprakash Mishra.

Each lab extends [xv6-riscv](https://github.com/mit-pdos/xv6-riscv), MIT's teaching
operating system, and each reference solution is one branch, starting from xv6-riscv
commit `06aad25`. Every commit on a branch is one step of the guided reveal on the lab's page.

| Branch | Lab |
|---|---|
| `ext/01-trace` | trace: logging system calls |
| `ext/02-backtrace` | A kernel backtrace on panic |
| `ext/03-procinfo` | ps: a safe snapshot of the process table |
| `ext/04-cputime` | CPU time accounting and a time command |
| `ext/05-sigalarm` | sigalarm and sigreturn: calling a user handler from a timer |
| `ext/06-ctrl-c` | Ctrl-C: interrupting the foreground job |
| `ext/07-profiler` | A sampling profiler |
| `ext/08-stride` | Stride scheduling with nice |
| `ext/09-threads` | User threads: clone, join and futex |
| `ext/10-vdso` | A shared read-only page: system calls without a trap |
| `ext/11-cow` | Copy-on-write fork |
| `ext/12-demand-exec` | Demand-paged exec |
| `ext/13-growstack` | A growable user stack |
| `ext/14-superpages` | Superpages |
| `ext/15-mmap` | mmap and munmap of files |
| `ext/16-swap` | Swapping to disk |
| `ext/17-sum` | Direct user access with sstatus.SUM |
| `ext/18-percpu-kalloc` | Per-CPU free lists in kalloc |
| `ext/19-bcache-hash` | A hashed buffer cache |
| `ext/20-ticketlock` | Ticket spinlocks |
| `ext/21-lockdep` | Lockdep-lite: a runtime lock-order checker |
| `ext/22-bigfile` | Doubly-indirect blocks: large files |
| `ext/23-symlink` | Symbolic links |
| `ext/24-rename` | Atomic rename |
| `ext/25-e1000` | An e1000 network driver and UDP sockets |

## Using it

Read the lab first and try it yourself. When you want the answer:

```sh
git clone https://github.com/ShowMeTheStack/xv6-riscv-labs
cd xv6-riscv-labs
git checkout -b my-cow 06aad25        # your own attempt, from the frozen commit
git diff 06aad25 origin/ext/11-cow    # the reference solution
git log --reverse 06aad25..origin/ext/11-cow   # its steps, one commit each
```

## License

- **xv6 itself** (every file as of `06aad25`, and the upstream history) is MIT, by Frans
  Kaashoek, Robert Morris, Russ Cox and the other xv6 authors.
- **The extension labs** (every change on the `ext/` branches relative to `06aad25`) are
  © 2026 Jyotiprakash Mishra, under the [GNU GPL v3.0 or later](LICENSE), with an additional
  term under section 7(b): derived works must keep the attribution
  *Based on the extension labs of "xv6, line by line" by Jyotiprakash Mishra
  (https://xv6.jyotiprakash.org)*.
- **The lab write-ups** on the website are CC BY-NC-SA 4.0.

[`NOTICE`](NOTICE) has the exact terms.
