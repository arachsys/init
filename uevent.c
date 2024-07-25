#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <sys/socket.h>

#define BUFFER 4096

static struct sockaddr_nl netlink = { .nl_family = AF_NETLINK };

static int broadcast(void) {
  char *action = NULL, *devpath = NULL, *event = NULL, *line = NULL;
  size_t length = 0, linesize = 0, size = 0;
  int sock, socksize = 1 << 21;
  ssize_t count;

  if ((sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT)) < 0)
    err(EXIT_FAILURE, "socket");

  setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &socksize, sizeof(int));
  setsockopt(sock, SOL_SOCKET, SO_SNDBUFFORCE, &socksize, sizeof(int));

  if (connect(sock, (struct sockaddr *) &netlink, sizeof(netlink)) < 0)
    err(EXIT_FAILURE, "connect");

  while ((count = getline(&line, &linesize, stdin)) > 0 || length) {
    if (line && count > 0 && line[count - 1] == '\n')
      count--;

    if (line && count > 0) {
      /* Append property line and null-terminate it. */
      while (size < length + count + 1)
        if (event = realloc(event, size += 4096), event == NULL)
          err(EXIT_FAILURE, "realloc");
      memcpy(event + length, line, count);
      event[length + count] = 0;

      /* Support both KEY VALUE and KEY=VALUE input lines. */
      for (size_t i = length; i < length + count; i++)
        if (event[i] == ' ' || event[i] == '=') {
          event[i] = '=';
          break;
        }

      /* Keep track of ACTION and DEVPATH for constructing header. */
      if (strncmp(event + length, "ACTION=", strlen("ACTION=")) == 0)
        action = event + length + strlen("ACTION=");
      if (strncmp(event + length, "DEVPATH=", strlen("DEVPATH=")) == 0)
        devpath = event + length + strlen("DEVPATH=");

      length += count + 1;
      continue;
    }

    if (action && devpath) {
      /* Make space for the ACTION@DEVPATH header then prepend it. */
      count = 2 + strlen(action) + strlen(devpath);

      while (size < length + count)
        if (event = realloc(event, size += 4096), event == NULL)
          err(EXIT_FAILURE, "realloc");

      memmove(event + count, event, length);
      snprintf(event, count, "%s@%s", action + count, devpath + count);

      /* Attempt to broadcast uevent but tolerate failures. */
      while (send(sock, event, length + count, 0) < 0)
        if (errno != EAGAIN && errno != EINTR)
          break;
    }
    action = devpath = NULL, length = 0;
  }

  free(line);
  close(sock);
  return ferror(stdin) ? EXIT_FAILURE : EXIT_SUCCESS;
}

void usage(char *progname) {
  fprintf(stderr, "\
Usage:\n\
  %1$s -l GROUPS  listen for uevents, printing them to stdout\n\
  %1$s -b GROUPS  read uevents from stdin and broadcast them\n\
", progname);
  exit(EX_USAGE);
}

int main(int argc, char **argv) {
  char buffer[BUFFER + 1], *cursor, *separator;
  int sock, socksize = 1 << 21;
  ssize_t length;

  if (argc == 3 && strcmp(argv[1], "-b") == 0) {
    netlink.nl_groups = strtoul(argv[2], NULL, 0);
    if (netlink.nl_groups == 0)
      errx(EXIT_FAILURE, "Invalid netlink group mask: %s", argv[2]);
    return broadcast();
  }

  if (argc == 3 && strcmp(argv[1], "-l") == 0) {
    netlink.nl_groups = strtoul(argv[2], NULL, 0);
    if (netlink.nl_groups == 0)
      errx(EXIT_FAILURE, "Invalid netlink group mask: %s", argv[2]);
  } else {
    usage(argv[0]);
  }

  if ((sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT)) < 0)
    err(EXIT_FAILURE, "socket");

  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &socksize, sizeof(int));
  setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &socksize, sizeof(int));

  if (bind(sock, (struct sockaddr *) &netlink, sizeof(netlink)) < 0)
    err(EXIT_FAILURE, "bind");

  putchar('\n');
  fflush(stdout);

  while (1) {
    if ((length = recv(sock, &buffer, sizeof(buffer) - 1, 0)) < 0) {
      if (errno == ENOBUFS) {
        printf("ACTION overflow\n\n");
        fflush(stdout);
      } else if (errno != EAGAIN && errno != EINTR) {
        err(EXIT_FAILURE, "recv");
      }
      continue;
    }

    /* Null-terminate the uevent and replace stray newlines with spaces. */
    buffer[length] = 0;
    for (cursor = buffer; cursor < buffer + length; cursor++)
      if (*cursor == '\n')
        *cursor = ' ';

    if (strlen(buffer) >= length - 1) {
      /* No properties; fake a simple environment based on the header. */
      if ((cursor = strchr(buffer, '@'))) {
        *cursor++ = 0;
        printf("ACTION %s\n", buffer);
        printf("DEVPATH %s\n", cursor);
      }
    } else {
      /* Ignore header as properties will include ACTION and DEVPATH. */
      cursor = buffer;
      while (cursor += strlen(cursor) + 1, cursor < buffer + length) {
        if ((separator = strchr(cursor, '=')))
          *separator = ' ';
        puts(cursor);
      }
    }
    putchar('\n');
    fflush(stdout);
  }
}
