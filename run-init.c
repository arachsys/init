#include <alloca.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6
#endif

#ifndef MS_MOVE
#define MS_MOVE 8192
#endif

static const char *program;

int nuke(const char *what);

int nuke_dirent(int len, const char *dir, const char *name, dev_t me) {
  int bytes = len + strlen(name) + 2;
  char path[bytes];
  int xlen;
  struct stat st;

  xlen = snprintf(path, bytes, "%s/%s", dir, name);
  assert(xlen < bytes);

  if (lstat(path, &st))
    return ENOENT;

  if (st.st_dev != me)
    return 0;

  return nuke(path);
}

static int nuke_dir(const char *what) {
  int len = strlen(what);
  DIR *dir;
  struct dirent *d;
  int err = 0;
  struct stat st;

  if (lstat(what, &st))
    return errno;

  if (!S_ISDIR(st.st_mode))
    return ENOTDIR;

  if (!(dir = opendir(what))) {
    return (errno == EACCES) ? 0 : errno;
  }

  while ((d = readdir(dir))) {
    if (d->d_name[0] == '.' &&
        (d->d_name[1] == '\0' ||
        (d->d_name[1] == '.' && d->d_name[2] == '\0')))
      continue;
    err = nuke_dirent(len, what, d->d_name, st.st_dev);
    if (err) {
      closedir(dir);
      return err;
    }
  }

  closedir(dir);
  return 0;
}

int nuke(const char *what) {
  int rv;
  int err = 0;

  rv = unlink(what);
  if (rv < 0) {
    if (errno == EISDIR) {
      err = nuke_dir(what);
      if (!err)
        err = rmdir(what) ? errno : err;
    } else {
      err = errno;
    }
  }

  if (err)
    error(1, err, what);
  return 0;
}

void usage(void) {
  fprintf(stderr, "Usage: exec %s [-c consoledev] /newroot /bin/init [args]\n",
          program);
  exit(1);
}

int main(int argc, char **argv) {
  struct stat rst, cst, ist;
  struct statfs sfs;
  int option, consolefd;

  const char *console = "/dev/console";
  const char *realroot;
  const char *init;
  char **initargs;

  program = argv[0];

  while ((option = getopt(argc, argv, "c:")) != -1) {
    if (option == 'c') {
      console = optarg;
    } else {
      usage();
    }
  }

  if (argc - optind < 2)
    usage();

  realroot = argv[optind];
  init = argv[optind + 1];
  initargs = argv + optind + 1;

  if (chdir(realroot))
    error(1, errno, "chdir to new root");

  if (stat("/", &rst) || stat(".", &cst))
    error(1, errno, "stat");

  if (rst.st_dev == cst.st_dev)
    error(1, 0, "Current directory is on same filesystem as new root");

  if (statfs("/", &sfs))
    error(1, errno, "statfs /");
  if (sfs.f_type != RAMFS_MAGIC && sfs.f_type != TMPFS_MAGIC)
    error(1, 0, "Current root is neither ramfs nor tmpfs");
  if (stat("/init", &ist) || !S_ISREG(ist.st_mode))
    error(1, 0, "/init not found in current root");

  if (nuke_dir("/"))
    error(1, 0, "Failed to clear out initramfs");

  if (mount(".", "/", NULL, MS_MOVE, NULL))
    error(1, 0, "Failed to move new root to /");

  if (chroot(".") || chdir("/"))
    error(1, 0, "chroot/chdir");

  if ((consolefd = open(console, O_RDWR)) < 0)
    error(1, errno, "opening console");
  dup2(consolefd, 0);
  dup2(consolefd, 1);
  dup2(consolefd, 2);
  close(consolefd);

  execv(init, initargs);
  error(1, errno, init);
  return EXIT_FAILURE; /* suppress gcc warning */
}
