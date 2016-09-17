#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

int main(int argc, char **argv) {
  int datasync, errc, fd;

  if (argc < 2) {
    fprintf(stderr, "Usage: %s FILE...\n", argv[0]);
    return EX_USAGE;
  }

  datasync = strstr(argv[0], "data") != NULL;

  for (argv++, argc--, errc = 0; argc > 0; argv++, argc--) {
    if ((fd = open(*argv, O_WRONLY)) < 0) {
      fprintf(stderr, "open %s: %s\n", *argv, strerror(errno));
      errc++;
      continue;
    }

    if ((datasync ? fdatasync : fsync)(fd) < 0) {
      fprintf(stderr, "%s %s: %s\n", datasync ? "fdatasync" : "fsync",
                                     *argv, strerror(errno));
      errc++;
    }

    close(fd);
  }

  return errc > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
