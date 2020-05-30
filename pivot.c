#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/syscall.h>

int main(int argc, char **argv) {
  int old, new;

  if (argc < 2) {
    fprintf(stderr, "Usage: %s NEW-ROOT [PUT-OLD]\n", argv[0]);
    return 64;
  }

  if (argc > 2 && strcmp(argv[1], argv[2]) != 0) {
    if (syscall(__NR_pivot_root, argv[1], argv[2]) < 0)
      err(EXIT_FAILURE, "cannot pivot to new root %s", argv[1]);
    return EXIT_SUCCESS;
  }

  if (old = open("/", O_DIRECTORY | O_RDONLY), old < 0)
    err(EXIT_FAILURE, "cannot open /");
  if (new = open(argv[1], O_DIRECTORY | O_RDONLY), new < 0)
    err(EXIT_FAILURE, "cannot open %s", argv[1]);
  if (fchdir(new) < 0 || syscall(__NR_pivot_root, ".", ".") < 0)
    err(EXIT_FAILURE, "cannot pivot to new root %s", argv[1]);

  if (fchdir(old) < 0)
    err(EXIT_FAILURE, "cannot re-enter old root");
  if (mount("", ".", "", MS_SLAVE | MS_REC, NULL) < 0)
    err(EXIT_FAILURE, "cannot disable old root mount propagation");
  if (umount2(".", MNT_DETACH) < 0)
    err(EXIT_FAILURE, "cannot detach old root");
  if (fchdir(new) < 0)
    err(EXIT_FAILURE, "cannot re-enter new root");

  return EXIT_SUCCESS;
}
