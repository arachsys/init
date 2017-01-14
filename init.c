#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/wait.h>

#define STARTUP_PROG "/etc/rc.startup"
#define SHUTDOWN_PROG "/etc/rc.shutdown"
#define SINGLE_PROG _PATH_BSHELL

static pid_t wait_pid = 0;
static char *progname, *shutdown_type = NULL;
static void (*shutdown_action)(void) = NULL;
static sigset_t mask_all, mask_default, mask_wait;

void console(void) {
  int fd;

  if ((fd = open("/dev/console", O_RDWR)) < 0)
    return;
  if (fd != STDIN_FILENO) {
    dup2(fd, STDIN_FILENO);
    close(fd);
  }
  dup2(STDIN_FILENO, STDOUT_FILENO);
  dup2(STDIN_FILENO, STDERR_FILENO);
  ioctl(STDIN_FILENO, TIOCSCTTY, 0);
}

void error(int status, int errnum, char *format, ...) {
  va_list args;

  fprintf(stderr, "%s: ", progname);
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  if (errnum != 0)
    fprintf(stderr, ": %s\n", strerror(errnum));
  else
    fputc('\n', stderr);
  if (status != 0)
    exit(status);
}

void final_reboot(void) {
  sigprocmask(SIG_SETMASK, &mask_all, NULL);
  if (fork() <= 0)
    reboot(RB_AUTOBOOT);
}

void final_halt(void) {
  sigprocmask(SIG_SETMASK, &mask_all, NULL);
  if (fork() <= 0)
    reboot(RB_HALT_SYSTEM);
}

void final_poweroff(void) {
  sigprocmask(SIG_SETMASK, &mask_all, NULL);
  if (fork() <= 0) {
    reboot(RB_POWER_OFF);
    reboot(RB_HALT_SYSTEM);
  }
}

void final_single(void) {
  console();
  sigprocmask(SIG_SETMASK, &mask_default, NULL);
  execl(SINGLE_PROG, SINGLE_PROG, NULL);
  error(0, errno, "exec %s", SINGLE_PROG);
  final_halt();
}

void shutdown_handler(int sig) {
  if (shutdown_type == NULL && wait_pid > 0)
    kill(wait_pid, SIGTERM);

  switch (sig) {
    case SIGTERM:
      shutdown_type = "single";
      shutdown_action = final_single;
      break;
    case SIGUSR1:
      shutdown_type = "halt";
      shutdown_action = final_halt;
      break;
    case SIGUSR2:
      shutdown_type = "poweroff";
      shutdown_action = final_poweroff;
      break;
    default: /* SIGINT */
      shutdown_type = "reboot";
      shutdown_action = final_reboot;
  }
}

void child_handler(int sig) {
  int status;
  pid_t pid;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    if (wait_pid == pid)
      wait_pid = 0;
}

pid_t run(char *filename, char **argv) {
  pid_t pid;

  switch (pid = fork()) {
    case -1:
      error(0, errno, "fork");
      return 0;
    case 0:
      setsid();
      console();
      sigprocmask(SIG_SETMASK, &mask_default, NULL);
      argv[0] = filename;
      execv(filename, argv);
      if (errno == ENOENT)
        exit(EXIT_SUCCESS);
      else
        error(EXIT_FAILURE, errno, "exec %s", filename);
    default:
      return pid;
  }
}

void shutdown(void) {
  char *argv[3];

  argv[0] = SHUTDOWN_PROG;
  argv[1] = shutdown_type;
  argv[2] = NULL;
  wait_pid = run(SHUTDOWN_PROG, argv);
  while (wait_pid != 0)
    sigsuspend(&mask_wait);
  sigprocmask(SIG_SETMASK, &mask_wait, NULL);
  kill(-1, SIGKILL);
  shutdown_action();
  pause();
}

int main(int argc, char **argv) {
  int fd;
  struct sigaction action;

  progname = argv[0];
  if (getpid() != 1)
    error(EXIT_FAILURE, 0, "Init must be run as process 1");

  fd = getdtablesize() - 1;
  while (fd >= 0)
    close(fd--);

  chdir("/");
  setsid();

  sigfillset(&action.sa_mask);
  action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  action.sa_handler = shutdown_handler;
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGUSR1, &action, NULL);
  sigaction(SIGUSR2, &action, NULL);

  action.sa_handler = child_handler;
  sigdelset(&action.sa_mask, SIGINT);
  sigdelset(&action.sa_mask, SIGTERM);
  sigdelset(&action.sa_mask, SIGUSR1);
  sigdelset(&action.sa_mask, SIGUSR2);
  sigaction(SIGCHLD, &action, NULL);

  sigfillset(&mask_all);
  sigfillset(&mask_wait);
  sigdelset(&mask_wait, SIGCHLD);
  sigdelset(&mask_wait, SIGINT);
  sigdelset(&mask_wait, SIGTERM);
  sigdelset(&mask_wait, SIGUSR1);
  sigdelset(&mask_wait, SIGUSR2);

  sigprocmask(SIG_SETMASK, &mask_all, &mask_default);
  wait_pid = run(STARTUP_PROG, argv);
  while (!shutdown_action || wait_pid != 0)
    sigsuspend(&mask_wait);
  shutdown();
  return EXIT_SUCCESS;
}
