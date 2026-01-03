#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>

const uint64_t fs_file = 0
  | LANDLOCK_ACCESS_FS_EXECUTE
  | LANDLOCK_ACCESS_FS_READ_FILE
  | LANDLOCK_ACCESS_FS_WRITE_FILE
  | LANDLOCK_ACCESS_FS_TRUNCATE;

const uint64_t fs_read = 0
  | LANDLOCK_ACCESS_FS_EXECUTE
  | LANDLOCK_ACCESS_FS_READ_FILE
  | LANDLOCK_ACCESS_FS_READ_DIR;

const uint64_t fs_write = 0
  | LANDLOCK_ACCESS_FS_WRITE_FILE
  | LANDLOCK_ACCESS_FS_TRUNCATE
  | LANDLOCK_ACCESS_FS_REMOVE_DIR
  | LANDLOCK_ACCESS_FS_REMOVE_FILE
  | LANDLOCK_ACCESS_FS_MAKE_CHAR
  | LANDLOCK_ACCESS_FS_MAKE_DIR
  | LANDLOCK_ACCESS_FS_MAKE_REG
  | LANDLOCK_ACCESS_FS_MAKE_SOCK
  | LANDLOCK_ACCESS_FS_MAKE_FIFO
  | LANDLOCK_ACCESS_FS_MAKE_BLOCK
  | LANDLOCK_ACCESS_FS_MAKE_SYM
  | LANDLOCK_ACCESS_FS_REFER;

static void allowpath(int ruleset, const char *path, char access) {
  struct landlock_path_beneath_attr attr = { 0 };
  struct stat status;

  if ((attr.parent_fd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC)) < 0)
    err(EXIT_FAILURE, "%s", path);
  if (fstat(attr.parent_fd, &status) < 0)
    err(EXIT_FAILURE, "fstat");

  attr.allowed_access = fs_read;
  if (access == 'w')
    attr.allowed_access |= fs_write;
  if (!S_ISDIR(status.st_mode))
    attr.allowed_access &= fs_file;

  if (syscall(__NR_landlock_add_rule, ruleset,
        LANDLOCK_RULE_PATH_BENEATH, &attr, 0) < 0)
    err(EXIT_FAILURE, "landlock_add_rule");

  close(attr.parent_fd);
}

static void allowport(int ruleset, const char *port, char access) {
  struct landlock_net_port_attr attr = { 0 };
  char *trailing;

  if (access == 't')
    attr.allowed_access = LANDLOCK_ACCESS_NET_BIND_TCP;
  if (access == 'T')
    attr.allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP;

  attr.port = strtoul(port, &trailing, 10);
  if (*port == 0 || *trailing != 0 || attr.port > 65535)
    errx(EXIT_FAILURE, "%s: Invalid port number", port);

  if (syscall(__NR_landlock_add_rule, ruleset,
        LANDLOCK_RULE_NET_PORT, &attr, 0) < 0)
    err(EXIT_FAILURE, "landlock_add_rule");
}

static int usage(char *progname) {
  fprintf(stderr, "\
Usage: %s [OPTIONS] CMD [ARG]...\n\
Options:\n\
  -d DIR    change directory to DIR before running CMD\n\
  -r PATH   allow CMD read-only access to PATH\n\
  -w PATH   allow CMD read-write access to PATH\n\
  -t PORT   allow CMD to listen on TCP port PORT\n\
  -T PORT   allow CMD to connect to TCP port PORT\n\
", progname);
  return 64;
}

int main(int argc, char **argv) {
  int option, ruleset;
  char *dir = NULL;

  if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) < 0)
    err(EXIT_FAILURE, "prctl PR_SET_NO_NEW_PRIVS");

  if ((ruleset = syscall(__NR_landlock_create_ruleset,
        &(struct landlock_ruleset_attr) {
          .handled_access_fs = fs_read | fs_write,
          .handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP
            | LANDLOCK_ACCESS_NET_CONNECT_TCP,
          .scoped = LANDLOCK_SCOPE_SIGNAL
            | LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
        }, sizeof(struct landlock_ruleset_attr), 0)) < 0)
    err(EXIT_FAILURE, "landlock_create_ruleset");

  while ((option = getopt(argc, argv, ":d:r:w:t:T:")) > 0)
    switch (option) {
      case 'd':
        dir = optarg;
        break;
      case 'r':
      case 'w':
        allowpath(ruleset, optarg, option);
        break;
      case 't':
      case 'T':
        allowport(ruleset, optarg, option);
        break;
      default:
        return usage(argv[0]);
    }

  if (optind >= argc)
    return usage(argv[0]);

  if (syscall(__NR_landlock_restrict_self, ruleset, 0) < 0)
    err(EXIT_FAILURE, "landlock_restrict_self");
  close(ruleset);

  if (dir && chdir(dir) < 0)
    err(EXIT_FAILURE, "%s", dir);
  execvp(argv[optind], argv + optind);
  err(EXIT_FAILURE, "execvp");
}
