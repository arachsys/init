#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sendfile.h>

const int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;

int main(int argc, char **argv, char **envp) {
  char *file, *path = getenv("PATH");
  int dst, end, src = -1;
  ssize_t length;

  if (argc < 2) {
    fprintf(stderr, "Usage: %s PROG [ARG]...\n", argv[0]);
    return 64;
  }

  if (strchr(argv[1], '/')) {
    if (access(file = argv[1], X_OK) < 0)
      err(EXIT_FAILURE, "%s", file);
    if ((src = open(file, O_RDONLY)) < 0)
      err(EXIT_FAILURE, "open %s", file);
  }

  while (src < 0 && path) {
    end = strcspn(path, ":");
    if (asprintf(&file, "%.*s%s%s", end, path, end ? "/" : "", argv[1]) < 0)
      err(EXIT_FAILURE, "malloc");
    if (access(file, X_OK) < 0)
      free(file);
    else if ((src = open(file, O_RDONLY)) < 0)
      err(EXIT_FAILURE, "open %s", file);
    path = path[end] ? path + end + 1 : NULL;
  }

  if (src < 0) {
    errno = ENOENT;
    err(EXIT_FAILURE, "%s", argv[1]);
  }

  if ((dst = memfd_create(file, MFD_CLOEXEC | MFD_ALLOW_SEALING)) < 0)
    err(EXIT_FAILURE, "memfd_create");

  while ((length = sendfile(dst, src, NULL, BUFSIZ)))
    if (length < 0 && errno != EAGAIN && errno != EINTR)
      err(EXIT_FAILURE, "sendfile");
  close(src);

  if (fcntl(dst, F_ADD_SEALS, seals) < 0)
    err(EXIT_FAILURE, "fcntl F_ADD_SEALS");
  fexecve(dst, argv + 1, envp);
  err(EXIT_FAILURE, "fexecve");
}
