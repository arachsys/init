#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

static struct {
  char **argv;
  id_t pid, gid, uid;
  int chdir, killed, restart;
} command;

static struct {
  char *priority, *tag;
  int file;
} logger;

static struct {
  char *path;
  int file;
} pidfile;

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

  if (!(event = malloc(sizeof(*event) + PATH_MAX + 1)))
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

void handler(int sig) {
  /* Don't restart command after SIGTERM. */
  if (sig == SIGTERM)
    command.restart = 0;
  /* Pass on HUP, INT, USR1, USR2, TERM signals to our child process. */
  if (command.pid > 0) {
    command.killed = 1;
    kill(command.pid, sig);
  }
}

void logger_setup(const char *spec) {
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
    logger.file = open(logger.tag, O_RDWR | O_APPEND | O_CREAT, 0666);
    if (logger.file < 0)
      err(EXIT_FAILURE, "%s", logger.tag);
    if (flock(logger.file, LOCK_EX | LOCK_NB) < 0)
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

void logger_start(void) {
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
    if (dup2(logger.file, STDOUT_FILENO) < 0)
      err(EXIT_FAILURE, "dup2");
    if (dup2(logger.file, STDERR_FILENO) < 0)
      err(EXIT_FAILURE, "dup2");
    if (logger.file != STDOUT_FILENO && logger.file != STDERR_FILENO)
      close(logger.file);
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

void pidfile_close(void) {
  if (pidfile.path) {
    close(pidfile.file);
    unlink(pidfile.path);
  }
}

void pidfile_open(const char *path) {
  pidfile.file = open(path, O_RDWR | O_CLOEXEC | O_CREAT, 0666);
  if (pidfile.file < 0)
    err(EXIT_FAILURE, "%s", path);
  if (flock(pidfile.file, LOCK_EX | LOCK_NB) < 0)
    errx(EXIT_FAILURE, "%s already locked", path);
  if (!(pidfile.path = realpath(path, NULL)))
    err(EXIT_FAILURE, "%s", path);
  atexit(pidfile_close);
  ftruncate(pidfile.file, 0);
}

void pidfile_write(void) {
  char *pid;
  int length;

  if (pidfile.path) {
    length = asprintf(&pid, "%ld\n", (long) getpid());
    if (length < 0)
      err(EXIT_FAILURE, "asprintf");
    if (write(pidfile.file, pid, length) < 0)
      err(EXIT_FAILURE, "write %s", pidfile.path);
    free(pid);
  }
}

void user_setup(char *name) {
  int count, tail;
  struct passwd *user;

  count = sscanf(name, "%u:%u%n", &command.uid, &command.gid, &tail);
  if (count < 2 || optarg[tail]) {
    if ((user = getpwnam(optarg))) {
      command.uid = user->pw_uid;
      command.gid = user->pw_gid;
    } else {
      errx(EXIT_FAILURE, "Invalid username");
    }
  }
}

void usage(char *progname) {
  fprintf(stderr, "\
Usage: %s [OPTIONS] CMD [ARG]...\n\
Options:\n\
  -c            change directory to the root before running the command\n\
  -l TAG:PRI    redirect stdout and stderr to a logger subprocess,\n\
                  using syslog tag TAG and priority/facility PRI\n\
  -l LOGFILE    append stdout and stderr to a file LOGFILE, which must be\n\
                  given as an absolute path whose first character is '/'\n\
  -p PIDFILE    lock PIDFILE and write pid to it, removing it on exit\n\
  -r            supervise the running command, restarting it if it dies\n\
                  and passing on TERM, INT, HUP, USR1 and USR2 signals\n\
  -u UID:GID    run the command with the specified numeric uid and gid\n\
  -u USERNAME   run the command with the uid and gid of user USERNAME\n\
  -w PATH       wait until PATH exists before running the command\n\
", progname);
  exit(EX_USAGE);
}

int main(int argc, char **argv) {
  char *options, *path;
  int fd, inotify, option, pwd, waitargs;
  pid_t pid;
  time_t started;
  struct sigaction action;

  /* Redirect stdin from /dev/null. */
  if ((fd = open("/dev/null", O_RDWR)) < 0)
    err(EXIT_FAILURE, "open /dev/null");
  if (fd != STDIN_FILENO)
    if ((dup2(fd, STDIN_FILENO)) < 0)
      err(EXIT_FAILURE, "dup2");

  /* Redirect stdout and/or stderr to /dev/null if closed. */
  while (fd <= STDERR_FILENO)
    if ((fd = dup(fd)) < 0)
      err(EXIT_FAILURE, "dup");
  close(fd);

  /* Close all file descriptors apart from stdin, stdout and stderr. */
  fd = getdtablesize() - 1;
  while (fd > STDERR_FILENO)
    close(fd--);

  options = "+:cfl:p:ru:w:", waitargs = 0;
  while ((option = getopt(argc, argv, options)) > 0)
    switch (option) {
      case 'c':
        command.chdir = 1;
        break;
      case 'f':
        /* On by default; ignored for compatibility with BSD daemon(1). */
        break;
      case 'l':
        logger_setup(optarg);
        break;
      case 'p':
        pidfile_open(optarg);
        break;
      case 'r':
        command.restart = 1;
        break;
      case 'u':
        user_setup(optarg);
        break;
      case 'w':
        waitargs++;
        break;
      default:
        usage(argv[0]);
    }

  if (argc <= optind)
    usage(argv[0]);

  switch (fork()) {
    case -1:
      err(EXIT_FAILURE, "fork");
    case 0:
      setsid(); /* This should work after forking; ignore errors anyway. */
      break;
    default:
      _exit(EXIT_SUCCESS); /* Don't delete pidfile in atexit() handler. */
  }

  logger_start();
  pidfile_write();

  /* We can handle all -w command line arguments now we're daemonized. */
  if (waitargs > 0) {
    if ((inotify = inotify_init1(IN_CLOEXEC)) < 0)
      err(EXIT_FAILURE, "inotify_init1");

    /* Open the working directory so we can restore it after each await(). */
    if ((pwd = open(".", O_RDONLY | O_DIRECTORY)) < 0)
      err(EXIT_FAILURE, "open pwd");

    optind = 0; /* Need to reset optind to reprocess arguments. */
    while ((option = getopt(argc, argv, options)) > 0)
      if (option == 'w') {
        if (!(path = strdup(optarg)))
          err(EXIT_FAILURE, "strdup");
        await(path, inotify, 0);
        free(path);
        fchdir(pwd);
      }

    close(inotify);
    close(pwd);
  }

  if (command.chdir && chdir("/") < 0)
    err(EXIT_FAILURE, "chdir");

  command.argv = argv + optind;
  if (!command.restart && !pidfile.path) {
    /* We don't need to supervise in this case, so just exec. */
    if (command.gid > 0 && setgid(command.gid) < 0)
      err(EXIT_FAILURE, "setgid");
    if (command.uid > 0 && setuid(command.uid) < 0)
      err(EXIT_FAILURE, "setuid");
    execvp(command.argv[0], command.argv);
    err(EXIT_FAILURE, "exec");
  }

  /* Handle and pass on HUP, INT, TERM, USR1, USR2 signals. */
  sigfillset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  action.sa_handler = handler;
  sigaction(SIGHUP, &action, NULL);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGUSR1, &action, NULL);
  sigaction(SIGUSR2, &action, NULL);

  do {
    command.killed = 0; /* Have we signalled the child? */
    switch (command.pid = fork()) {
      case -1:
        err(EXIT_FAILURE, "fork");
      case 0:
        if (command.gid > 0 && setgid(command.gid) < 0)
          err(EXIT_FAILURE, "setgid");
        if (command.uid > 0 && setuid(command.uid) < 0)
          err(EXIT_FAILURE, "setuid");
        setsid(); /* This should work after forking; ignore errors anyway. */
        execvp(command.argv[0], command.argv);
        err(EXIT_FAILURE, "exec");
    }

    started = time(NULL);
    while (pid = wait(NULL), pid != (pid_t) command.pid)
      if (pid < 0 && errno != EINTR)
        err(EXIT_FAILURE, "wait");

    /* Try to avoid restarting a crashing command in a tight loop. */
    if (command.restart && !command.killed && time(NULL) < started + 5)
      errx(EXIT_FAILURE, "Child died within 5 seconds: not restarting");
  } while (command.restart);

  return EXIT_SUCCESS;
}
