#define _GNU_SOURCE
#define SYSLOG_NAMES
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <features.h>
#include <poll.h>
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

#define BUFFER 65536

#ifndef UTCLOG
/* syslog(3) time stamps are UTC from musl and local time from glibc. */
#ifdef __GLIBC__
#define UTCLOG 0
#else
#define UTCLOG 1
#endif
#endif

static char buffer[BUFFER + 1], *zone;

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

  (zone && zone[0] ? localtime_r : gmtime_r)(&now, date);
  return cursor ? cursor - line : 0;
}

static char *syslog_facility(int priority) {
  for (size_t i = 0; facilitynames[i].c_val >= 0; i++)
    if (facilitynames[i].c_val == (priority & LOG_FACMASK))
      return facilitynames[i].c_name;
  return "unknown"; /* facility not found in facilitynames[] */
}

static int syslog_priority(char *line, int *priority) {
  int start = 0;

  sscanf(line, "<%d>%n", priority, &start);
  return start;
}

int syslog_open(void) {
  int fd;
  struct sockaddr_un addr = {
    .sun_family = AF_UNIX,
    .sun_path = "/dev/log"
  };

  if ((fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0)) < 0)
    err(EXIT_FAILURE, "socket");

  unlink(addr.sun_path);
  umask(0111); /* Syslog socket should be writeable by everyone. */
  if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    err(EXIT_FAILURE, "bind %s", addr.sun_path);

  if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &(int) { 1 },
        sizeof(int)) < 0)
    err(EXIT_FAILURE, "setsockopt SO_PASSCRED %s", addr.sun_path);
  return fd;
}

static void syslog_recv(int fd) {
  char *cursor;
  int length, priority;
  struct iovec block;
  struct msghdr header;
  struct tm date;
  struct ucred id;
  union {
    struct cmsghdr hdr;
    char data[CMSG_SPACE(sizeof(struct ucred))];
  } cmsg;

  block.iov_base = buffer;
  block.iov_len = sizeof(buffer) - 1;
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

  for (cursor = buffer; cursor < buffer + length; cursor++)
    if (*cursor == 0 || *cursor == '\n')
      *cursor = 0;
    else if ((*cursor < 32 && *cursor != '\t') || *cursor == 127)
      *cursor = ' ';
  buffer[length] = 0;

  cursor = buffer;
  priority = LOG_DAEMON | LOG_NOTICE;

  while (cursor < buffer + length) {
    cursor += syslog_priority(cursor, &priority);
    cursor += syslog_date(cursor, &date);

    if (*cursor) {
      if (zone && zone[0])
        printf("%u %u %u %s %u %04u-%02u-%02u %02u:%02u:%02u%c%02u%02u %s\n",
          id.pid, id.uid, id.gid, syslog_facility(priority),
          priority & LOG_PRIMASK, date.tm_year + 1900, date.tm_mon + 1,
          date.tm_mday, date.tm_hour, date.tm_min, date.tm_sec,
          date.tm_gmtoff < 0 ? '-' : '+', abs((int) date.tm_gmtoff) / 3600,
          abs((int) date.tm_gmtoff) / 60 % 60, cursor);
      else
        printf("%u %u %u %s %u %04u-%02u-%02u %02u:%02u:%02u %s\n", id.pid,
          id.uid, id.gid, syslog_facility(priority), priority & LOG_PRIMASK,
          date.tm_year + 1900, date.tm_mon + 1, date.tm_mday, date.tm_hour,
          date.tm_min, date.tm_sec, cursor);
    }
    cursor += strlen(cursor) + 1;
  }
  fflush(stdout);
}

static void kernel_read(int fd) {
  char *cursor;
  int length, priority;
  struct tm date;
  time_t now;

  if ((length = read(fd, buffer, sizeof(buffer) - 1)) <= 0)
    return;

  time(&now);
  (zone && zone[0] ? localtime_r : gmtime_r)(&now, &date);

  for (cursor = buffer; cursor < buffer + length; cursor++)
    if (*cursor == 0 || *cursor == '\n')
      *cursor = 0;
    else if ((*cursor < 32 && *cursor != '\t') || *cursor == 127)
      *cursor = ' ';
  buffer[length] = 0;

  priority = strtoul(buffer, &cursor, 10);
  if (cursor == buffer)
    priority = LOG_KERN | LOG_NOTICE;

  cursor = strchr(buffer, ';');
  cursor = cursor ? cursor + 1 : buffer;

  if (*cursor) {
    if (zone && zone[0])
      printf("0 0 0 %s %u %04u-%02u-%02u %02u:%02u:%02u%c%02u%02u %s\n",
        syslog_facility(priority), priority & LOG_PRIMASK,
        date.tm_year + 1900, date.tm_mon + 1, date.tm_mday, date.tm_hour,
        date.tm_min, date.tm_sec, date.tm_gmtoff < 0 ? '-' : '+',
        abs((int) date.tm_gmtoff) / 3600,
        abs((int) date.tm_gmtoff) / 60 % 60, cursor);
    else
      printf("0 0 0 %s %u %04u-%02u-%02u %02u:%02u:%02u %s\n",
        syslog_facility(priority), priority & LOG_PRIMASK,
        date.tm_year + 1900, date.tm_mon + 1, date.tm_mday, date.tm_hour,
        date.tm_min, date.tm_sec, cursor);
    fflush(stdout);
  }
}

int main(int argc, char **argv) {
  struct pollfd fds[2];

  if ((fds[0].fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK)) < 0)
    err(EXIT_FAILURE, "open /dev/kmsg");
  fds[1].fd = syslog_open();

  zone = getenv("TZ");

  fds[0].events = fds[1].events = POLLIN;
  while(1) {
    while (poll(fds, 2, -1) < 0)
      if (errno != EAGAIN && errno != EINTR)
        err(EXIT_FAILURE, "poll");
    if (fds[0].revents & POLLIN)
      kernel_read(fds[0].fd);
    if (fds[1].revents & POLLIN)
      syslog_recv(fds[1].fd);
  }
}
