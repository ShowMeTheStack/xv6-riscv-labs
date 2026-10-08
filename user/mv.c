#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// mv old new: give old the name new. If new is an existing
// directory, move old into it under its own last name.
int
main(int argc, char *argv[])
{
  char buf[128], *new, *base, *p;
  struct stat st;

  if (argc != 3) {
    fprintf(2, "Usage: mv old new\n");
    exit(1);
  }
  new = argv[2];
  if (stat(new, &st) == 0 && st.type == T_DIR) {
    base = argv[1];
    for (p = argv[1]; *p; p++)
      if (*p == '/' && p[1] != '\0')
        base = p + 1;
    if (strlen(new) + 1 + strlen(base) + 1 > sizeof(buf)) {
      fprintf(2, "mv: path too long\n");
      exit(1);
    }
    strcpy(buf, new);
    p = buf + strlen(buf);
    *p++ = '/';
    strcpy(p, base);
    new = buf;
  }
  if (rename(argv[1], new) < 0) {
    fprintf(2, "mv %s %s: failed\n", argv[1], new);
    exit(1);
  }
  exit(0);
}
