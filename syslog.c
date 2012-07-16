#define _XOPEN_SOURCE 500
#define SYSLOG_NAMES
#include <ctype.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
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

int main(void) {
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

  if (!(kernel.data = malloc(kernel.size = BUFFER + 1)))
    error(EXIT_FAILURE, errno, "malloc");
  if (!(syslog.data = malloc(syslog.size = BUFFER + 1)))
    error(EXIT_FAILURE, errno, "malloc");

  if ((fds[0].fd = open(KERNEL, O_RDONLY)) < 0)
    error(EXIT_FAILURE, errno, "open %s", KERNEL);
  kernel.length = 0;

  if ((fds[1].fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
    error(EXIT_FAILURE, errno, "socket");
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SYSLOG);
  unlink(addr.sun_path);
  if (bind(fds[1].fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)))
    error(EXIT_FAILURE, errno, "bind %s", addr.sun_path);

  fds[0].events = fds[1].events = POLLIN;
  while(1) {
    if (poll(fds, 2, -1) < 0)
      error(EXIT_FAILURE, errno, "poll");
    if (fds[0].revents & POLLIN)
      if (!kernel_read(fds[0].fd, kernel.data, &kernel.length, kernel.size))
        fds[0].events = 0;
    if (fds[1].revents & POLLIN)
      syslog_recv(fds[1].fd, syslog.data, syslog.size);
  }

  return EXIT_FAILURE;
}
