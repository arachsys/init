#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
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
} logger;

static struct {
  char *path;
  FILE *file;
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

void handler(int sig) {
  /* Don't restart command after SIGTERM. */
  if (sig == SIGTERM)
    command.restart = 0;
  /* Pass on HUP, INT, USR1, USR2, TERM signals to our child process. */
  if (command.pid > 0) {
    command.killed = 1;
    kill(command.pid, sig);
  }
  return;
}

void logger_setup(const char *spec) {
  int status;
  pid_t pid;

  if (logger.tag)
    error(EXIT_FAILURE, 0, "-l cannot be specified more than once");
  if (!*spec || *spec == ':')
    error(EXIT_FAILURE, 0, "Invalid or missing syslog identifier tag");
  if (!(logger.tag = strdup(spec)))
    error(EXIT_FAILURE, errno, "strdup");

  /* Log spec format is TAG:PRIORITY. */
  if ((logger.priority = strchr(logger.tag, ':'))) {
    *logger.priority++ = 0;
    if (!*logger.priority)
      logger.priority = NULL;
  }

  /* Test the logger settings so we can exit early if they are invalid. */
  switch (pid = fork()) {
    case -1:
      error(EXIT_FAILURE, errno, "fork");
    case 0:
      if (chdir("/") < 0)
        error(EXIT_FAILURE, errno, "chdir");
      close(STDIN_FILENO);
      execlp("logger", "logger",
          "-p", logger.priority ? logger.priority : "daemon.notice",
          "-t", logger.tag, NULL);
      error(EXIT_FAILURE, errno, "exec");
  }

  if (waitpid(pid, &status, 0) < 0)
    error(EXIT_FAILURE, errno, "waitpid");

  /* The logger subprocess writes its own message to stderr on failure. */
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    exit(1);
}

void logger_start(void) {
  int logpipe[2];

  /* Don't try to do anything if the logger hasn't been configured. */
  if (!logger.tag)
    return;

  if (pipe(logpipe) < 0)
    error(EXIT_FAILURE, errno, "pipe");
  switch (fork()) {
    case -1:
      error(EXIT_FAILURE, errno, "fork");
    case 0:
      /* Don't unintentionally keep the pwd busy in the logger process. */
      if (chdir("/") < 0)
        error(EXIT_FAILURE, errno, "chdir");
      /* Run logger(1) with stdin coming from the read end of the pipe. */
      if (dup2(logpipe[0], STDIN_FILENO) < 0)
        error(EXIT_FAILURE, errno, "dup2");
      close(logpipe[0]);
      close(logpipe[1]);
      execlp("logger", "logger",
          "-p", logger.priority ? logger.priority : "daemon.notice",
          "-t", logger.tag, NULL);
      error(EXIT_FAILURE, errno, "exec");
  }

  /* Redirect our stdout and stderr to the write end of the pipe. */
  if (dup2(logpipe[1], STDOUT_FILENO) < 0)
    error(EXIT_FAILURE, errno, "dup2");
  if (dup2(logpipe[1], STDERR_FILENO) < 0)
    error(EXIT_FAILURE, errno, "dup2");
  close(logpipe[0]);
  close(logpipe[1]);
  return;
}

void pidfile_close(void) {
  if (pidfile.file)
    fclose(pidfile.file);
  if (pidfile.path)
    unlink(pidfile.path);
  return;
}

void pidfile_open(const char *path) {
  if (!(pidfile.file = fopen(path, "a+")))
    error(EXIT_FAILURE, errno, "%s", path);
  if (flock(fileno(pidfile.file), LOCK_EX | LOCK_NB) < 0)
    error(EXIT_FAILURE, 0, "%s already locked", path);
  if (!(pidfile.path = realpath(path, NULL)))
    error(EXIT_FAILURE, errno, "%s", path);
  atexit(pidfile_close);
  ftruncate(fileno(pidfile.file), 0);
  rewind(pidfile.file);
  return;
}

void pidfile_write(void) {
  if (pidfile.file) {
    fprintf(pidfile.file, "%ld\n", (long) getpid());
    fflush(pidfile.file);
  }
  return;
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
      error(EXIT_FAILURE, 0, "Invalid username");
    }
  }
  return;
}

void usage(char *progname) {
  fprintf(stderr, "\
Usage: %s [OPTIONS] CMD [ARG]...\n\
Options:\n\
  -c            change directory to the root before running the command\n\
  -l TAG:PRI    redirect stdout and stderr to a logger subprocess,\n\
                  using syslog tag TAG and priority/facility PRI\n\
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
  int inotify, option, pwd, waitargs;
  pid_t pid;
  time_t started;
  struct sigaction action;

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

  daemon(1, 0);
  logger_start();
  pidfile_write();

  /* We can handle all -w command line arguments now we're daemonized. */
  if (waitargs > 0) {
    if ((inotify = inotify_init1(IN_CLOEXEC)) < 0)
      error(EXIT_FAILURE, errno, "inotify_init1");

    /* Open the working directory so we can restore it after each await(). */
    if ((pwd = open(".", O_RDONLY | O_DIRECTORY)) < 0)
      error(EXIT_FAILURE, errno, "open pwd");

    optind = 0; /* Need to reset optind to reprocess arguments. */
    while ((option = getopt(argc, argv, options)) > 0)
      if (option == 'w') {
        if (!(path = strdup(optarg)))
          error(EXIT_FAILURE, errno, "strdup");
        await(path, inotify, 0);
        free(path);
        fchdir(pwd);
      }

    close(inotify);
    close(pwd);
  }

  if (command.chdir && chdir("/") < 0)
    error(EXIT_FAILURE, errno, "chdir");

  command.argv = argv + optind;
  if (!command.restart && !pidfile.file) {
    /* We don't need to supervise in this case, so just exec. */
    if (command.gid > 0 && setgid(command.gid) < 0)
      error(EXIT_FAILURE, errno, "setgid");
    if (command.uid > 0 && setuid(command.uid) < 0)
      error(EXIT_FAILURE, errno, "setuid");
    execvp(command.argv[0], command.argv);
    error(EXIT_FAILURE, errno, "exec");
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
        error(EXIT_FAILURE, errno, "fork");
      case 0:
        if (command.gid > 0 && setgid(command.gid) < 0)
          error(EXIT_FAILURE, errno, "setgid");
        if (command.uid > 0 && setuid(command.uid) < 0)
          error(EXIT_FAILURE, errno, "setuid");
        execvp(command.argv[0], command.argv);
        error(EXIT_FAILURE, errno, "exec");
    }

    started = time(NULL);
    while (pid = wait(NULL), pid != (pid_t) command.pid)
      if (pid < 0 && errno != EINTR)
        error(EXIT_FAILURE, errno, "wait");

    /* Try to avoid restarting a crashing command in a tight loop. */
    if (command.restart && !command.killed && time(NULL) < started + 5)
      error(EXIT_FAILURE, 0, "Child died within 5 seconds: not restarting");
  } while (command.restart);

  return EXIT_SUCCESS;
}
