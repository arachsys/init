#define _BSD_SOURCE
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>

void await(const char *path, int inotify, int parent) {
  char *slash;
  int watch;
  struct inotify_event *event;
  struct stat test;

  /* Take a short-cut if path already exists and is a parent dir. */
  if (parent && !chdir(path))
    return;

  /* If leading slashes are present, chdir to the root and remove them. */
  if (*path == '/') {
    if (chdir("/"))
      error(EXIT_FAILURE, errno, "chdir /");
    while (*path == '/')
      path++;
  }

  /* Remove any stray trailing slashes. */
  slash = strrchr(path, '/');
  if (slash && slash[1] == 0) {
    while (*slash == '/')
      *slash-- = 0;
    slash = strrchr(path, '/');
  }

  /* Recurse to await the parent dir if necessary. */
  if (slash) {
    *slash = 0;
    await(path, inotify, 1);
    path = slash + 1;
  }

  if (*path == 0)
    return;

  /* Now wait for the correct leaf name to arrive in our working dir. */
  if ((watch = inotify_add_watch(inotify, ".", IN_CREATE | IN_MOVED_TO)) < 0)
    error(EXIT_FAILURE, errno, "inotify_add_watch");

  /* Check if it already exists after setting watch to avoid a race. */
  if (parent) {
    if (!chdir(path))
      goto out;
    if (errno != ENOENT)
      error(EXIT_FAILURE, errno, "chdir %s", path);
  } else {
    if (!stat(path, &test))
      goto out;
    if (errno != ENOENT)
      error(EXIT_FAILURE, errno, "stat %s", path);
  }

  if (!(event = malloc(sizeof(*event) + PATH_MAX + 1)))
    error(EXIT_FAILURE, errno, "malloc");

  /* Otherwise, wait for a matching create/move-into event. */
  while(1) {
    if (read(inotify, event, sizeof(*event) + PATH_MAX + 1) < 0) {
      if (errno != EAGAIN && errno != EINTR)
        error(EXIT_FAILURE, errno, "read");
    } else if (!strcmp(path, event->name)) {
      if (!parent || !chdir(path))
        goto out;
      if (errno != ENOENT)
        error(EXIT_FAILURE, errno, "chdir %s", path);
    }
  }

out:
  inotify_rm_watch(inotify, watch);
  return;
}

int main(int argc, char **argv) {
  char *path;
  int inotify, pathc, pwd;

  if (argc < 2) {
    fprintf(stderr, "Usage: %s [PATH]... [ -- CMD [ARG]... ]\n", argv[0]);
    return EX_USAGE;
  }

  if ((inotify = inotify_init1(IN_CLOEXEC)) < 0)
    error(EXIT_FAILURE, errno, "inotify_init1");

  if ((pwd = open(".", O_RDONLY | O_DIRECTORY)) < 0)
    error(EXIT_FAILURE, errno, "open pwd");

  for (argv++, argc--, pathc = 0; pathc < argc; pathc++)
    if (!strcmp(argv[pathc], "--"))
      break;

  if (argc > pathc + 1)
    daemon(1, 0);

  while (argc--, pathc-- > 0) {
    if (!(path = strdup(*argv++)))
      error(EXIT_FAILURE, errno, "strdup");
    await(path, inotify, 0);
    free(path);
    fchdir(pwd);
  }

  close(inotify);
  close(pwd);

  if (argc > 1) {
    chdir("/");
    execvp(argv[1], argv + 1);
    error(EXIT_FAILURE, errno, "exec");
  }

  return EXIT_SUCCESS;
}
