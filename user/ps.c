// ps: list the processes, from one procinfo() snapshot.

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/pinfo.h"
#include "user/user.h"

static char *states[] = {
  [1] = "used", [2] = "sleep", [3] = "runble", [4] = "run", [5] = "zombie",
};

struct pinfo buf[NPROC];

// print s, then spaces up to width w.
void
col(char *s, int w)
{
  int n = strlen(s);

  printf("%s", s);
  while (n++ < w)
    printf(" ");
}

// format a small non-negative int, or "-" for -1.
char *
itoa(int x, char *s)
{
  char t[12];
  int i = 0, j = 0;

  if (x < 0)
    return "-";
  do
    t[i++] = '0' + x % 10;
  while ((x /= 10) != 0);
  while (i > 0)
    s[j++] = t[--i];
  s[j] = 0;
  return s;
}

int
main(void)
{
  char a[12];
  int i, n;

  n = procinfo(buf, NPROC);
  if (n < 0) {
    printf("ps: procinfo failed\n");
    exit(1);
  }
  printf("PID   PPID  STATE   CPU  SIZE    NAME\n");
  for (i = 0; i < n; i++) {
    col(itoa(buf[i].pid, a), 6);
    col(itoa(buf[i].ppid, a), 6);
    if (buf[i].state >= 1 && buf[i].state <= 5)
      col(states[buf[i].state], 8);
    else
      col("???", 8);
    col(itoa(buf[i].cpu, a), 5);
    col(itoa((int)buf[i].sz, a), 8);
    printf("%s\n", buf[i].name);
  }
  exit(0);
}
