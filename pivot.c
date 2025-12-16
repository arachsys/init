#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/syscall.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s NEW-ROOT [PUT-OLD]\n", argv[0]);
    return 64;
  }

  if (argc > 2) {
    if (syscall(__NR_pivot_root, argv[1], argv[2]) < 0)
      err(EXIT_FAILURE, "cannot pivot to new root %s", argv[1]);
    return EXIT_SUCCESS;
  }

  if (chdir(argv[1]) < 0 || syscall(__NR_pivot_root, ".", ".") < 0)
    err(EXIT_FAILURE, "cannot pivot to new root %s", argv[1]);
  if (mount(NULL, ".", NULL, MS_SLAVE | MS_REC, NULL) < 0)
    err(EXIT_FAILURE, "cannot disable old root mount propagation");
  if (umount2(".", MNT_DETACH) < 0)
    err(EXIT_FAILURE, "cannot detach old root");

  return EXIT_SUCCESS;
}
