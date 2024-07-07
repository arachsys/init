#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

static id_t gid, uid;
static size_t listeners;
static struct inotify_event *event;
static struct pollfd *pollfd;
static int signals[2];

static struct {
  char *priority, *tag;
  int fd;
} logger;

static struct {
  char *path;
  int fd;
} pidfile;

static void await(const char *path, int inotify, int parent) {
  struct stat test;
  char *slash;
  int watch;

  /* Take a short-cut if path already exists and is a parent dir. */
  if (parent && !chdir(path))
    return;

  /* If leading slashes are present, chdir to the root and remove them. */
  if (*path == '/') {
    if (chdir("/"))
      err(EXIT_FAILURE, "chdir /");
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
    err(EXIT_FAILURE, "inotify_add_watch");

  /* Check if it already exists after setting watch to avoid a race. */
  if (parent) {
    if (!chdir(path))
      goto out;
    if (errno != ENOENT)
      err(EXIT_FAILURE, "chdir %s", path);
  } else {
    if (!stat(path, &test))
      goto out;
    if (errno != ENOENT)
      err(EXIT_FAILURE, "stat %s", path);
  }

  if (event == NULL)
    event = malloc(sizeof(*event) + PATH_MAX + 1);
  if (event == NULL)
    err(EXIT_FAILURE, "malloc");

  /* Otherwise, wait for a matching create/move-into event. */
  while(1) {
    if (read(inotify, event, sizeof(*event) + PATH_MAX + 1) < 0) {
      if (errno != EAGAIN && errno != EINTR)
        err(EXIT_FAILURE, "read");
    } else if (!strcmp(path, event->name)) {
      if (!parent || !chdir(path))
        goto out;
      if (errno != ENOENT)
        err(EXIT_FAILURE, "chdir %s", path);
    }
  }

out:
  inotify_rm_watch(inotify, watch);
}

static void listen_add(int fd) {
  if ((listeners & 15) == 0) {
    pollfd = realloc(pollfd, (listeners + 16) * sizeof(struct pollfd));
    if (pollfd == NULL)
      err(EXIT_FAILURE, "realloc");
  }
  pollfd[listeners++].fd = fd;
}

static void listen_tcp(const char *address) {
  struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *info, *list;
  char host[256], port[32];
  int fd, status;

  if (sscanf(address, "[%255[^]]]:%31[^:]", host, port) != 2) {
    if (sscanf(address, "%255[^:]:%31[^:]", host, port) != 2) {
      if (sscanf(address, ":%31[^:]", port) != 1)
        errx(EXIT_FAILURE, "%s: Invalid address", address);
      snprintf(host, sizeof(host), "::");
    }
  }

  if ((status = getaddrinfo(host, port, &hints, &list)) != 0)
    errx(EXIT_FAILURE, "getaddrinfo: %s", gai_strerror(status));

  for (info = list; info != NULL; info = info->ai_next) {
    if ((fd = socket(info->ai_family, info->ai_socktype, 0)) < 0)
      err(EXIT_FAILURE, "socket");
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int) { 1 }, sizeof(int));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    if (bind(fd, info->ai_addr, info->ai_addrlen) < 0)
      err(EXIT_FAILURE, "bind");
    if (listen(fd, SOMAXCONN) < 0)
      err(EXIT_FAILURE, "listen");
    listen_add(fd);
  }
}

static void listen_unix(const char *path) {
  struct sockaddr_un address;
  size_t length = strlen(path);
  int fd;

  /* On Linux, address.sun_path is NUL-padded not NUL-terminated. */
  if (length > sizeof(address.sun_path))
    errx(EXIT_FAILURE, "Socket path is too long to bind");
  length += offsetof(struct sockaddr_un, sun_path);

  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, path, sizeof(address.sun_path));
  unlink(path);

  if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
    err(EXIT_FAILURE, "socket");
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  fcntl(fd, F_SETFL, O_NONBLOCK);

  if (bind(fd, (struct sockaddr *) &address, length) < 0)
    err(EXIT_FAILURE, "bind");
  if (listen(fd, SOMAXCONN) < 0)
    err(EXIT_FAILURE, "listen");
  listen_add(fd);
}

static void logger_setup(const char *spec) {
  int status;
  pid_t pid;

  if (logger.tag)
    errx(EXIT_FAILURE, "-l cannot be specified more than once");
  if (!*spec || *spec == ':')
    errx(EXIT_FAILURE, "Invalid or missing syslog identifier tag");
  if (!(logger.tag = strdup(spec)))
    err(EXIT_FAILURE, "strdup");

  /* Logging to file indicated by absolute path. */
  if (*logger.tag == '/') {
    logger.fd = open(logger.tag, O_RDWR | O_APPEND | O_CREAT, 0666);
    if (logger.fd < 0)
      err(EXIT_FAILURE, "%s", logger.tag);
    if (flock(logger.fd, LOCK_EX | LOCK_NB) < 0)
      errx(EXIT_FAILURE, "%s already locked", logger.tag);
    return;
  }

  /* Log spec format is TAG:PRIORITY. */
  if ((logger.priority = strchr(logger.tag, ':'))) {
    *logger.priority++ = 0;
    if (!*logger.priority)
      logger.priority = NULL;
  }

  /* Test the logger settings so we can exit early if they are invalid. */
  switch (pid = fork()) {
    case -1:
      err(EXIT_FAILURE, "fork");
    case 0:
      if (chdir("/") < 0)
        err(EXIT_FAILURE, "chdir");
      execlp("logger", "logger", "-f", "/dev/null",
          "-p", logger.priority ? logger.priority : "daemon.notice",
          "-t", logger.tag, NULL);
      err(EXIT_FAILURE, "exec");
  }

  if (waitpid(pid, &status, 0) < 0)
    err(EXIT_FAILURE, "waitpid");

  /* The logger subprocess writes its own message to stderr on failure. */
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    exit(EXIT_FAILURE);
}

static void logger_start(void) {
  int logpipe[2];

  /* Redirect stdout and stderr to /dev/null if logging isn't configured. */
  if (!logger.tag) {
    if (dup2(STDIN_FILENO, STDOUT_FILENO) < 0)
      err(EXIT_FAILURE, "dup2");
    if (dup2(STDIN_FILENO, STDERR_FILENO) < 0)
      err(EXIT_FAILURE, "dup2");
    return;
  }

  /* Redirect stdout and stderr if a log file has been specified. */
  if (*logger.tag == '/') {
    if (dup2(logger.fd, STDOUT_FILENO) < 0)
      err(EXIT_FAILURE, "dup2");
    if (dup2(logger.fd, STDERR_FILENO) < 0)
      err(EXIT_FAILURE, "dup2");
    if (logger.fd != STDOUT_FILENO && logger.fd != STDERR_FILENO)
      close(logger.fd);
    return;
  }

  if (pipe(logpipe) < 0)
    err(EXIT_FAILURE, "pipe");
  switch (fork()) {
    case -1:
      err(EXIT_FAILURE, "fork");
    case 0:
      /* Don't unintentionally keep the pwd busy in the logger process. */
      if (chdir("/") < 0)
        err(EXIT_FAILURE, "chdir");
      /* Redirect stdout and stderr to /dev/null for the logger process. */
      if (dup2(STDIN_FILENO, STDOUT_FILENO) < 0)
        err(EXIT_FAILURE, "dup2");
      if (dup2(STDIN_FILENO, STDERR_FILENO) < 0)
        err(EXIT_FAILURE, "dup2");
      /* Run logger(1) with stdin coming from the read end of the pipe. */
      if (dup2(logpipe[0], STDIN_FILENO) < 0)
        err(EXIT_FAILURE, "dup2");
      close(logpipe[0]);
      close(logpipe[1]);
      execlp("logger", "logger",
          "-p", logger.priority ? logger.priority : "daemon.notice",
          "-t", logger.tag, NULL);
      err(EXIT_FAILURE, "exec");
  }

  /* Redirect our stdout and stderr to the write end of the pipe. */
  if (dup2(logpipe[1], STDOUT_FILENO) < 0)
    err(EXIT_FAILURE, "dup2");
  if (dup2(logpipe[1], STDERR_FILENO) < 0)
    err(EXIT_FAILURE, "dup2");
  close(logpipe[0]);
  close(logpipe[1]);
}

static void pidfile_close(void) {
  if (pidfile.path) {
    close(pidfile.fd);
    unlink(pidfile.path);
  }
}

static void pidfile_open(const char *path) {
  pidfile.fd = open(path, O_RDWR | O_CLOEXEC | O_CREAT, 0666);
  if (pidfile.fd < 0)
    err(EXIT_FAILURE, "%s", path);
  if (flock(pidfile.fd, LOCK_EX | LOCK_NB) < 0)
    errx(EXIT_FAILURE, "%s already locked", path);
  if (!(pidfile.path = realpath(path, NULL)))
    err(EXIT_FAILURE, "%s", path);
  if (ftruncate(pidfile.fd, 0) < 0)
    err(EXIT_FAILURE, "%s", path);
  atexit(pidfile_close);
}

static void pidfile_write(void) {
  if (pidfile.path && dprintf(pidfile.fd, "%d\n", getpid()) < 0)
    err(EXIT_FAILURE, "dprintf");
}

static pid_t reap(int *status) {
  pid_t child;

  while ((child = waitpid(-1, status, WNOHANG)) < 0)
    if (errno != EINTR)
      break;
  return child > 0 ? child : 0;
}

static int signal_get(void) {
  unsigned char signal = 0;
  while (read(signals[0], &signal, 1) < 0)
    if (errno != EINTR)
      break;
  return signal;
}

static void signal_put(int signal) {
  while (write(signals[1], &(unsigned char) { signal }, 1) < 0)
    if (errno != EINTR)
      break;
}

static void execute(char **argv) {
  if (gid > 0 && setgid(gid) < 0)
    err(EXIT_FAILURE, "setgid");
  if (uid > 0 && setuid(uid) < 0)
    err(EXIT_FAILURE, "setuid");
  execvp(argv[0], argv);
  err(EXIT_FAILURE, "exec");
}

static int serve(char **argv, size_t limit) {
  int connection;
  size_t count = 0;

  listen_add(signals[0]);
  pollfd[listeners - 1].events = POLLIN;

  while (1) {
    /* Only listen for new connections when below the connection limit. */
    for (size_t i = 0; i + 1 < listeners; i++)
      pollfd[i].events = count < limit ? POLLIN : 0;

    if (poll(pollfd, listeners, -1) < 0) {
      if (errno != EINTR && errno != EAGAIN)
        err(EXIT_FAILURE, "poll");
      continue;
    }

    /* Deal with signals first in case they free additional slots. */
    if (pollfd[listeners - 1].revents & POLLIN)
      switch (signal_get()) {
        case SIGCHLD:
          while (reap(NULL) > 0)
            if (count > 0)
              count--;
          break;
        case SIGINT:
        case SIGTERM:
          return EXIT_SUCCESS;
      }

    /* Accept connections from ready listeners until we hit our limit. */
    for (size_t i = 0; i + 1 < listeners; i++)
      if (pollfd[i].revents & POLLIN && count < limit)
        if ((connection = accept(pollfd[i].fd, NULL, NULL)) >= 0) {
          switch (fork()) {
            case -1:
              break;
            case 0:
              if (dup2(connection, STDIN_FILENO) < 0)
                err(EXIT_FAILURE, "dup2");
              if (dup2(connection, STDOUT_FILENO) < 0)
                err(EXIT_FAILURE, "dup2");
              close(connection);
              execute(argv);
            default:
              count++;
          }
          close(connection);
        }
  }
}

static int supervise(char **argv, int restart) {
  int signal;
  pid_t child, command;
  time_t wait;

  do {
    switch (command = fork()) {
      case -1:
        err(EXIT_FAILURE, "fork");
      case 0:
        setsid(); /* Ignore errors but should always work after fork. */
        execute(argv);
    }

    wait = time(NULL) + 5;
    while (command)
      switch (signal = signal_get()) {
        case SIGCHLD:
          /* Reap every child, watching out for the command pid. */
          while ((child = reap(NULL)))
            if (child == command)
              command = 0;
          break;
        case SIGTERM:
          restart = 0;
          /* Fall through to the default behaviour. */
        case SIGHUP:
        case SIGINT:
        case SIGUSR1:
        case SIGUSR2:
          /* Pass signals on to our child process. */
          kill(command, signal);
          wait = 0;
      }

    /* Try to avoid restarting a crashing command in a tight loop. */
    if (restart && time(NULL) < wait)
      errx(EXIT_FAILURE, "Child died within 5 seconds: not restarting");
  } while (restart);

  return EXIT_SUCCESS;
}

static void usage(char *progname) {
  fprintf(stderr, "\
Usage: %s [OPTIONS] CMD [ARG]...\n\
Options:\n\
  -d DIR        change directory to DIR before running the command\n\
  -f            fork twice so the command is not a session leader\n\
  -l TAG:PRI    redirect stdout and stderr to a logger subprocess,\n\
                  using syslog tag TAG and priority/facility PRI\n\
  -l LOGFILE    append stdout and stderr to a file LOGFILE, which must be\n\
                  given as an absolute path whose first character is '/'\n\
  -n LIMIT      allow no more than LIMIT concurrent socket connections\n\
  -p PIDFILE    lock PIDFILE and write pid to it, removing it on exit\n\
  -r            supervise the running command, restarting it if it dies\n\
                  and passing on TERM, INT, HUP, USR1 and USR2 signals\n\
  -s PATH       listen on a unix stream socket and run the command with\n\
                  stdin and stdout attached to each connection\n\
  -t HOST:PORT  listen on a TCP stream socket and run the command with\n\
                  stdin and stdout attached to each connection\n\
  -u UID:GID    run the command with the specified numeric uid and gid\n\
  -u USERNAME   run the command with the uid and gid of user USERNAME\n\
  -w PATH       wait until PATH exists before running the command\n\
", progname);
  exit(EX_USAGE);
}

int main(int argc, char **argv) {
  char *dir = NULL, *options, *path;
  int fd, inotify, option, pwd, tail, waitargs;
  size_t doublefork = 0, limit = -1, restart = 0;
  struct passwd *user;

  /* Redirect stdin from /dev/null. */
  if ((fd = open("/dev/null", O_RDWR)) < 0)
    err(EXIT_FAILURE, "open /dev/null");
  if (fd != STDIN_FILENO) {
    if ((dup2(fd, STDIN_FILENO)) < 0)
      err(EXIT_FAILURE, "dup2");
    close(fd);
  }

  /* Redirect stdout and/or stderr to /dev/null if closed. */
  if (fcntl(STDOUT_FILENO, F_GETFD) < 0 && errno == EBADF)
    if ((dup2(STDIN_FILENO, STDOUT_FILENO)) < 0)
      err(EXIT_FAILURE, "dup2");
  if (fcntl(STDERR_FILENO, F_GETFD) < 0 && errno == EBADF)
    if ((dup2(STDIN_FILENO, STDERR_FILENO)) < 0)
      err(EXIT_FAILURE, "dup2");

  options = "+:cd:fl:n:p:rs:t:u:w:", waitargs = 0;
  while ((option = getopt(argc, argv, options)) > 0)
    switch (option) {
      case 'c':
        /* Special case of -d DIR, for compatibility with BSD daemon(1). */
        dir = "/";
        break;
      case 'd':
        dir = optarg;
        if ((fd = open(dir, O_RDONLY | O_DIRECTORY)) < 0)
          err(EXIT_FAILURE, "%s", dir);
        close(fd);
        break;
      case 'f':
        doublefork = 1;
        break;
      case 'l':
        logger_setup(optarg);
        break;
      case 'n':
        if (sscanf(optarg, "%zu%n", &limit, &tail) >= 1)
          if (optarg[tail] == 0)
            break;
        errx(EXIT_FAILURE, "Invalid connection limit");
      case 'p':
        pidfile_open(optarg);
        break;
      case 'r':
        restart = 1;
        break;
      case 's':
        listen_unix(optarg);
        break;
      case 't':
        listen_tcp(optarg);
        break;
      case 'u':
        if (sscanf(optarg, "%u:%u%n", &uid, &gid, &tail) >= 2)
          if (optarg[tail] == 0)
            break;
        if ((user = getpwnam(optarg))) {
          uid = user->pw_uid;
          gid = user->pw_gid;
          break;
        }
        errx(EXIT_FAILURE, "Invalid username");
      case 'w':
        waitargs++;
        break;
      default:
        usage(argv[0]);
    }

  /* When run with just -w arguments, we await paths in the foreground. */
  if (waitargs > 0 && argc == 2 * waitargs + 1)
    goto await;

  if (argc <= optind)
    usage(argv[0]);

  /* Fork into the background then create a session and process group. */
  switch (fork()) {
    case -1:
      err(EXIT_FAILURE, "fork");
    case 0:
      setsid(); /* Ignore errors but should always work after fork. */
      break;
    default:
      _exit(EXIT_SUCCESS); /* Don't delete pidfile in atexit() handler. */
  }

  if (doublefork) {
    /* Fork again to ensure we are not the session leader. */
    switch (fork()) {
      case -1:
        err(EXIT_FAILURE, "fork");
      case 0:
        break;
      default:
        _exit(EXIT_SUCCESS); /* Don't delete pidfile in atexit() handler. */
    }
  }

  logger_start();
  pidfile_write();

await:
  if (waitargs > 0) {
    if ((inotify = inotify_init1(IN_CLOEXEC)) < 0)
      err(EXIT_FAILURE, "inotify_init1");

    /* Open the working directory so we can restore it after each await(). */
    if ((pwd = open(".", O_RDONLY | O_DIRECTORY)) < 0)
      err(EXIT_FAILURE, "open pwd");

    optind = 0; /* Need to reset optind to reprocess -w arguments. */
    while ((option = getopt(argc, argv, options)) > 0)
      if (option == 'w') {
        if (!(path = strdup(optarg)))
          err(EXIT_FAILURE, "strdup");
        await(path, inotify, 0);
        free(path);
        if (fchdir(pwd) < 0)
          err(EXIT_FAILURE, "fchdir");
      }

    close(inotify);
    close(pwd);
  }

  /* Exit if we were just awaiting paths in the foreground. */
  if (argc <= optind)
    return EXIT_SUCCESS;

  if (dir && chdir(dir) < 0)
    err(EXIT_FAILURE, "chdir");

  /* If we don't need to supervise it, just exec the command. */
  if (!restart && !pidfile.path && !listeners)
    execute(argv + optind);

  /* Use a signals pipe to avoid async-unsafe handlers. */
  if (pipe2(signals, O_CLOEXEC) < 0)
    err(EXIT_FAILURE, "pipe");

  /* Avoid using SIG_IGN as this disposition persists across exec. */
  signal(SIGHUP, signal_put);
  signal(SIGINT, signal_put);
  signal(SIGPIPE, signal_put);
  signal(SIGTERM, signal_put);
  signal(SIGCHLD, signal_put);
  signal(SIGUSR1, signal_put);
  signal(SIGUSR2, signal_put);

  if (listeners > 0)
    return serve(argv + optind, limit);
  return supervise(argv + optind, restart);
}
