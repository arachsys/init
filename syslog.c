#define _GNU_SOURCE
#define SYSLOG_NAMES
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <features.h>
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

#ifndef UTCLOG
/* syslog(3) time stamps are UTC from musl and local time from glibc. */
#ifdef __GLIBC__
#define UTCLOG 0
#else
#define UTCLOG 1
#endif
#endif

#ifndef UTC
#define UTC 1
#endif

static pid_t pid;
static char *progname;

static void handler(int signal) {
  if (signal != SIGPIPE && pid > 0)
    kill(pid, signal);
  if (signal == SIGPIPE || signal == SIGTERM)
    _exit(EXIT_SUCCESS);
}

static void subprocess(char **argv) {
  int logpipe[2];

  if (pipe(logpipe) < 0)
    err(EXIT_FAILURE, "pipe");

  signal(SIGCHLD, SIG_IGN);
  switch (pid = fork()) {
    case -1:
      err(EXIT_FAILURE, "fork");
    case 0:
      if (dup2(logpipe[0], STDIN_FILENO) < 0)
        err(EXIT_FAILURE, "dup2");
      close(logpipe[0]);
      close(logpipe[1]);
      execvp(argv[0], argv);
      err(EXIT_FAILURE, "exec");
  }

  if (dup2(logpipe[1], STDOUT_FILENO) < 0)
    err(EXIT_FAILURE, "dup2");
  close(logpipe[0]);
  close(logpipe[1]);

  /* Pass on HUP, INT, TERM, USR1, USR2; exit on TERM or PIPE. */
  signal(SIGHUP, handler);
  signal(SIGINT, handler);
  signal(SIGPIPE, handler);
  signal(SIGTERM, handler);
  signal(SIGUSR1, handler);
  signal(SIGUSR2, handler);

  argv[0] = NULL;
}

static int syslog_date(char *line, struct tm *date) {
  char *cursor;
  time_t now, offset;

  time(&now);
  (UTCLOG ? gmtime_r : localtime_r)(&now, date);
  if ((cursor = strptime(line, " %b %d %H:%M:%S ", date))) {
    /* Pick tm_year so the timestamp is closest to now. */
    offset = now - (UTCLOG ? timegm : mktime)(date);
    date->tm_year += (offset - 15778800) / 31557600;
    now = (UTCLOG ? timegm : mktime)(date);
  }

  (UTC ? gmtime_r : localtime_r)(&now, date);
  return cursor ? cursor - line : 0;
}

static char *syslog_facility(int priority) {
  char *facility;
  int value;

  facility = "unknown"; /* facility not found in facilitynames[] */
  for (value = 0; facilitynames[value].c_val >= 0; value++)
    if (facilitynames[value].c_val == (priority & LOG_FACMASK))
      facility = facilitynames[value].c_name;
  return facility;
}

static int syslog_priority(char *line, char **facility, int *level) {
  int priority, start;

  start = 0, sscanf(line, "<%d>%n", &priority, &start);
  if (start > 0) {
    *facility = syslog_facility(priority);
    *level = LOG_PRI(priority);
  }

  return start;
}

static void syslog_recv(int fd, char *data, size_t size) {
  char *cursor, *facility;
  int level, length;
  struct iovec block;
  struct msghdr header;
  struct tm date;
  struct ucred id;
  union {
    struct cmsghdr hdr;
    char data[CMSG_SPACE(sizeof(struct ucred))];
  } cmsg;

  block.iov_base = data;
  block.iov_len = size - 1;
  header.msg_name = NULL;
  header.msg_namelen = 0;
  header.msg_iov = &block;
  header.msg_iovlen = 1;
  header.msg_control = &cmsg;
  header.msg_controllen = sizeof(cmsg);
  header.msg_flags = 0;

  if ((length = recvmsg(fd, &header, 0)) <= 0)
    return;

  id.pid = id.uid = id.gid = 0;
  if (cmsg.hdr.cmsg_level == SOL_SOCKET)
    if (cmsg.hdr.cmsg_type == SCM_CREDENTIALS)
      memcpy(&id, CMSG_DATA(&cmsg.hdr), sizeof(struct ucred));

  for (cursor = data; cursor < data + length; cursor++)
    if (*cursor == 0 || *cursor == '\n')
      *cursor = 0;
    else if ((*cursor < 32 && *cursor != '\t') || *cursor == 127)
      *cursor = ' ';
  data[length] = 0;

  cursor = data, facility = syslog_facility(LOG_USER), level = LOG_NOTICE;
  while (cursor < data + length) {
    cursor += syslog_priority(cursor, &facility, &level);
    cursor += syslog_date(cursor, &date);
    if (*cursor) {
#if UTC
      printf("%u %u %u %s %u %04u-%02u-%02u %02u:%02u:%02u %s\n", id.pid,
          id.uid, id.gid, facility, level, date.tm_year + 1900,
          date.tm_mon + 1, date.tm_mday, date.tm_hour, date.tm_min,
          date.tm_sec, cursor);
#else
      printf("%u %u %u %s %u %04u-%02u-%02u %02u:%02u:%02u %c%02u%02u %s\n",
          id.pid, id.uid, id.gid, facility, level, date.tm_year + 1900,
          date.tm_mon + 1, date.tm_mday, date.tm_hour, date.tm_min,
          date.tm_sec, date.tm_gmtoff < 0 ? '-' : '+',
          abs(date.tm_gmtoff / 3600), abs(date.tm_gmtoff / 60 % 60), cursor);
#endif
    }
    cursor += strlen(cursor) + 1;
  }
  fflush(stdout);
}

static int kernel_print(char *line) {
  char *facility;
  int level, start;
  struct tm date;
  time_t now;

  time(&now);
  (UTC ? gmtime_r : localtime_r)(&now, &date);

  facility = syslog_facility(LOG_KERN), level = LOG_NOTICE;
  start = syslog_priority(line, &facility, &level);

  if (line[start]) {
#if UTC
    printf("0 0 0 %s %u %04u-%02u-%02u %02u:%02u:%02u %s\n", facility, level,
        date.tm_year + 1900, date.tm_mon + 1, date.tm_mday, date.tm_hour,
        date.tm_min, date.tm_sec, line + start);
#else
    printf("0 0 0 %s %u %04u-%02u-%02u %02u:%02u:%02u %c%02u%02u %s\n",
        facility, level, date.tm_year + 1900, date.tm_mon + 1,
        date.tm_mday, date.tm_hour, date.tm_min, date.tm_sec,
        date.tm_gmtoff < 0 ? '-' : '+', abs(date.tm_gmtoff / 3600),
        abs(date.tm_gmtoff / 60 % 60), line + start);
#endif
  }

  return start;
}

static size_t kernel_read(int fd, char *data, size_t *length, size_t size) {
  char *cursor;
  ssize_t new;

  if ((new = read(fd, data + *length, size - *length - 1)) <= 0)
    return 0;

  for (cursor = data + *length; cursor < data + *length + new; cursor++)
    if (*cursor == 0 || *cursor == '\n')
      *cursor = 0;
    else if ((*cursor < 32 && *cursor != '\t') || *cursor == 127)
      *cursor = ' ';
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
    err(EXIT_FAILURE, "malloc");
  if (!(syslog.data = malloc(syslog.size = BUFFER + 1)))
    err(EXIT_FAILURE, "malloc");

  if ((fds[0].fd = open(KERNEL, O_RDONLY)) < 0)
    err(EXIT_FAILURE, "open %s", KERNEL);
  kernel.length = 0;

  if ((fds[1].fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
    err(EXIT_FAILURE, "socket");
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SYSLOG);
  unlink(addr.sun_path);
  umask(0111); /* Syslog socket should be writeable by everyone. */
  if (bind(fds[1].fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    err(EXIT_FAILURE, "bind %s", addr.sun_path);

  if (setsockopt(fds[1].fd, SOL_SOCKET, SO_PASSCRED, &(int) { 1 },
        sizeof(int)) < 0)
    err(EXIT_FAILURE, "setsockopt SO_PASSCRED");

  fds[0].events = fds[1].events = POLLIN;
  while(1) {
    while (poll(fds, 2, -1) < 0)
      if (errno != EAGAIN && errno != EINTR)
        err(EXIT_FAILURE, "poll");
    if (fds[0].revents & POLLIN)
      if (!kernel_read(fds[0].fd, kernel.data, &kernel.length, kernel.size))
        fds[0].events = 0;
    if (fds[1].revents & POLLIN)
      syslog_recv(fds[1].fd, syslog.data, syslog.size);
  }

  return EXIT_FAILURE;
}
