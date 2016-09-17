#define _GNU_SOURCE
#define SYSLOG_NAMES
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifndef BUFFER
#define BUFFER 65536
#endif

#ifndef KERNEL
#define KERNEL "/proc/kmsg"
#endif

#ifndef SYSLOG
#define SYSLOG "/dev/log"
#endif

#ifndef UTC
#define UTC 1
#endif

static pid_t pid;
static char *progname;

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

void handler(int sig) {
  if (pid > 0)
    kill(pid, sig);
  if (sig == SIGTERM)
    exit(EXIT_SUCCESS);
  return;
}

void subprocess(char **argv) {
  int logpipe[2];
  struct sigaction action;

  if (pipe(logpipe) < 0)
    error(EXIT_FAILURE, errno, "pipe");

  signal(SIGCHLD, SIG_IGN);
  switch (pid = fork()) {
    case -1:
      error(EXIT_FAILURE, errno, "fork");
    case 0:
      if (dup2(logpipe[0], STDIN_FILENO) < 0)
        error(EXIT_FAILURE, errno, "dup2");
      close(logpipe[0]);
      close(logpipe[1]);
      execvp(argv[0], argv);
      error(EXIT_FAILURE, errno, "exec");
  }

  if (dup2(logpipe[1], STDOUT_FILENO) < 0)
    error(EXIT_FAILURE, errno, "dup2");
  close(logpipe[0]);
  close(logpipe[1]);

  /* Pass on HUP, INT, TERM, USR1, USR2 signals, and exit on SIGTERM. */
  sigfillset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  action.sa_handler = handler;
  sigaction(SIGHUP, &action, NULL);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGUSR1, &action, NULL);
  sigaction(SIGUSR2, &action, NULL);

  argv[0] = NULL;
  return;
}

int kernel_print(char *line) {
  int level, start;
  struct tm date;
  time_t now;

  time(&now);
  (UTC ? gmtime_r : localtime_r)(&now, &date);
  start = 0, sscanf(line, " <%d> %n", &level, &start);

  if (line[start])
    printf("kern %d %04d-%02d-%02d %02d:%02d:%02d kernel: %s\n",
        start > 0 ? level : 5, date.tm_year + 1900, date.tm_mon + 1,
        date.tm_mday, date.tm_hour, date.tm_min, date.tm_sec, line + start);

  return start;
}

size_t kernel_read(int fd, char *data, size_t *length, size_t size) {
  char *cursor;
  ssize_t new;

  if ((new = read(fd, data + *length, size - *length - 1)) <= 0)
    return 0;

  for (cursor = data + *length; cursor < data + *length + new; cursor++)
    *cursor = isprint(*cursor) ? *cursor : 0;
  *length += new;

  while ((cursor = memchr(data, 0, *length))) {
    kernel_print(data);
    *length = data + *length - cursor - 1;
    memmove(data, cursor + 1, *length);
  }

  if (*length >= size - 1) {
    data[size - 1] = 0;
    *length = kernel_print(data);
  }

  fflush(stdout);
  return new;
}

int syslog_date(char *line, struct tm *date) {
  char *cursor;
  time_t now;

  time(&now);
  if ((cursor = strptime(line, " %b %d %H:%M:%S ", date))) {
    /* Syslog reports timestamps in local time. */
    date->tm_isdst = -1;

    /* Pick tm_year so mktime(time) is closest to now. */
    date->tm_year = localtime(&now)->tm_year;
    if (mktime(date) > now + 15778800)
      date->tm_year--;
    else if (mktime(date) < now - 15778800)
      date->tm_year++;

    now = mktime(date);
  }

  (UTC ? gmtime_r : localtime_r)(&now, date);
  return cursor ? cursor - line : 0;
}

int syslog_priority(char *line, char **facility, int *level) {
  int priority, start, value;

  start = 0, sscanf(line, " <%d> %n", &priority, &start);
  if (start > 0) {
    for (value = 0; facilitynames[value].c_val >= 0; value++)
      if (facilitynames[value].c_val == (priority & LOG_FACMASK))
        *facility = facilitynames[value].c_name;
    *level = LOG_PRI(priority);
  }

  return start;
}

void syslog_recv(int fd, char *data, size_t size) {
  char *cursor, *facility;
  int level, length;
  struct tm date;

  if ((length = recv(fd, data, size - 1, 0)) <= 0)
    return;

  for (cursor = data; cursor < data + length; cursor++)
    *cursor = isprint(*cursor) ? *cursor : 0;
  data[length] = 0;

  facility = "user";
  level = 5;

  for (cursor = data; cursor < data + length; cursor += strlen(cursor) + 1) {
    cursor += syslog_priority(cursor, &facility, &level);
    cursor += syslog_date(cursor, &date);
    if (*cursor)
      printf("%s %d %04d-%02d-%02d %02d:%02d:%02d %s\n", facility, level,
          date.tm_year + 1900, date.tm_mon + 1, date.tm_mday, date.tm_hour,
          date.tm_min, date.tm_sec, cursor);
  }
  fflush(stdout);
}

int main(int argc, char **argv) {
  struct pollfd fds[2];
  struct sockaddr_un addr;

  struct {
    char *data;
    size_t length, size;
  } kernel;

  struct {
    char *data;
    size_t size;
  } syslog;

  progname = argv[0];
  if (argc > 1)
    subprocess(argv + 1);

  if (!(kernel.data = malloc(kernel.size = BUFFER + 1)))
    error(EXIT_FAILURE, errno, "malloc");
  if (!(syslog.data = malloc(syslog.size = BUFFER + 1)))
    error(EXIT_FAILURE, errno, "malloc");

  if ((fds[0].fd = open(KERNEL, O_RDONLY)) < 0)
    error(EXIT_FAILURE, errno, "open %s", KERNEL);
  kernel.length = 0;

  if ((fds[1].fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
    error(EXIT_FAILURE, errno, "socket");
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SYSLOG);
  unlink(addr.sun_path);
  umask(0111); /* Syslog socket should be writeable by everyone. */
  if (bind(fds[1].fd, (struct sockaddr *) &addr, sizeof(addr)))
    error(EXIT_FAILURE, errno, "bind %s", addr.sun_path);

  fds[0].events = fds[1].events = POLLIN;
  while(1) {
    while (poll(fds, 2, -1) < 0)
      if (errno != EAGAIN && errno != EINTR)
        error(EXIT_FAILURE, errno, "poll");
    if (fds[0].revents & POLLIN)
      if (!kernel_read(fds[0].fd, kernel.data, &kernel.length, kernel.size))
        fds[0].events = 0;
    if (fds[1].revents & POLLIN)
      syslog_recv(fds[1].fd, syslog.data, syslog.size);
  }

  return EXIT_FAILURE;
}
