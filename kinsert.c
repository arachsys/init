#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/module.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

static int getfile(char *filename) {
  ssize_t chunk, offset, size;
  char buffer[BUFSIZ];
  struct stat status;
  int fd, memfd;

  if ((fd = open(filename, O_RDONLY)) < 0)
    err(EXIT_FAILURE, "open %s", filename);
  if (fstat(fd, &status) >= 0 && S_ISREG(status.st_mode))
    return fd;

  if ((memfd = memfd_create(filename, 0)) < 0)
    err(EXIT_FAILURE, "memfd_create");

  while (offset = 0, size = read(fd, buffer, sizeof buffer)) {
    if (size < 0) {
      if (errno != EAGAIN && errno != EINTR)
        err(EXIT_FAILURE, "read %s", filename);
      continue;
    }
    while (offset < size) {
      if ((chunk = write(memfd, buffer + offset, size - offset)) >= 0)
        offset += chunk;
      else if (errno != EAGAIN && errno != EINTR)
        err(EXIT_FAILURE, "write memfd");
    }
  }

  if (lseek(memfd, 0, SEEK_SET) < 0)
    err(EXIT_FAILURE, "lseek");
  close(fd);
  return memfd;
}

static int usage(char *progname) {
  if (strstr(progname, "remove"))
    fprintf(stderr, "Usage: %s MODULE...\n", progname);
  else
    fprintf(stderr, "Usage: %s MODULE [PARAM]\n", progname);
  return 64;
}

int main(int argc, char **argv) {
  int flags = 0, force = 0, length = 0, module, option, status;
  char *params;

  while ((option = getopt(argc, argv, ":f")) > 0)
    switch (option) {
      case 'f':
        force = 1;
        break;
      default:
        return usage(argv[0]);
    }

  if (optind >= argc)
    return usage(argv[0]);

  if (strstr(argv[0], "remove")) {
    flags |= O_NONBLOCK | (force ? O_TRUNC : 0);
    for (status = EXIT_SUCCESS; optind < argc; optind++)
      if (syscall(__NR_delete_module, argv[optind], flags) < 0) {
        warn("delete_module %s", argv[optind]);
        status = EXIT_FAILURE;
      }
    return status;
  }

  if (force) {
    flags |= MODULE_INIT_IGNORE_MODVERSIONS;
    flags |= MODULE_INIT_IGNORE_VERMAGIC;
  }

  module = getfile(argv[optind]);
  for (size_t i = optind + 1; i < argc; i++)
    length += strlen(argv[i]) + 1;
  params = calloc(length ? length : 1, 1);

  for (size_t i = optind + 1, j = 0; i < argc; i++) {
    if (j > 0)
      params[j++] = ' ';
    j = stpcpy(params + j, argv[i]) - params;
  }

  if (syscall(__NR_finit_module, module, params, flags) < 0)
    err(EXIT_FAILURE, "finit_module %s", argv[optind]);
  return EXIT_SUCCESS;
}
