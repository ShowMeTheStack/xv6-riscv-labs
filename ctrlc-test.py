#!/usr/bin/env python3

#
# Types Ctrl-C at a booted xv6 and checks what happens.
#
# ./ctrlc-test.py [kernel/kernel [fs.img]] [-- extra shell commands]
#
# Boots QEMU directly (3 harts), types commands and \x03 bytes on
# its standard input (the UART), and prints one OK/FAIL line per
# check. Extra commands after -- are run at the end (for example
# pgtest or "usertests -q"), and their output is only shown.
#

import os, select, subprocess, sys, time

args = sys.argv[1:]
extra = []
if "--" in args:
    i = args.index("--")
    args, extra = args[:i], args[i + 1:]
kernel = args[0] if len(args) > 0 else "kernel/kernel"
fsimg = args[1] if len(args) > 1 else "fs.img"

q = subprocess.Popen(
    ["qemu-system-riscv64", "-machine", "virt", "-bios", "none",
     "-kernel", kernel, "-m", "128M", "-smp", "3", "-nographic",
     "-global", "virtio-mmio.force-legacy=false",
     "-drive", f"file={fsimg},if=none,format=raw,id=x0",
     "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

out = b""  # everything the console printed
seen = 0   # how much of out the checks have consumed


def pump(t):
    global out
    r, _, _ = select.select([q.stdout], [], [], t)
    if r:
        d = os.read(q.stdout.fileno(), 65536)
        if d:
            out += d
            sys.stdout.buffer.write(d)
            sys.stdout.flush()


def expect(pat, tmo):
    """Wait for pat after the last match; True if it came in time."""
    global seen
    pat = pat.encode()
    end = time.time() + tmo
    while time.time() < end:
        i = out.find(pat, seen)
        if i >= 0:
            seen = i + len(pat)
            return True
        pump(0.1)
    return False


def quiet(t):
    end = time.time() + t
    while time.time() < end:
        pump(0.1)


def type_(s):
    q.stdin.write(s.encode())
    q.stdin.flush()


results = []


def check(ok, what):
    results.append((ok, what))
    print(f"\n### ctrlc-test: {what}: {'OK' if ok else 'FAIL'}", flush=True)


assert expect("$ ", 60), "no shell prompt"
start = time.time()

# 1. a job reading the console
type_("cat\n")
quiet(1)
type_("\x03")
check(expect("^C\n$ ", 5), "cat waiting for input, then ^C")

# 2. a job asleep in pause (100 s)
type_("pause 1000\n")
quiet(1)
t = time.time()
type_("\x03")
ok = expect("^C\n$ ", 5)
check(ok, f"pause 1000, then ^C (prompt after {time.time() - t:.3f} s)")

# 3. a pipeline: cat in consoleread, grep in piperead, their parent in wait
type_("cat | grep x\n")
quiet(1)
type_("\x03")
check(expect("^C\n$ ", 5), "cat | grep x, then ^C")

# 4. a background job survives a ^C for the foreground job
type_("(pause 30; echo survived) &\n")
check(expect("$ ", 5), "background job started")
type_("cat\n")
quiet(1)
type_("\x03")
check(expect("^C\n$ ", 5), "cat, then ^C, with a background job running")
check(expect("survived\n", 10), "the background job finished its work")

# 5. ^C at the prompt: no job, the shell must not die
type_("\x03")
check(expect("^C\n$ ", 5), "^C at an empty prompt gives a new prompt")
type_("echo jun")
quiet(0.5)
type_("\x03")
quiet(0.5)
type_("echo clean\n")
# the reference prompts straight after the ^C line; had the line
# survived, the shell would have run "echo jun" and printed jun there.
check(expect("echo jun^C\n$ ", 5) and expect("clean\n$ ", 5), "^C drops a half-typed line")
type_("echo alive\n")
check(expect("alive\n$ ", 5), "the shell still runs commands")
check(out.count(b"init: starting sh") == 1, "the shell was never restarted")

for c in extra:
    type_(c + "\n")
    expect("$ ", 3600)

q.kill()
bad = [w for ok, w in results if not ok]
print(f"\nctrlc-test: {len(results) - len(bad)} of {len(results)} checks OK", flush=True)
print("ctrlc-test: " + ("ALL OK" if not bad else "SOME CHECKS FAILED"))
sys.exit(1 if bad else 0)
