#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <sys/socket.h>

#define BUFFER 4096

int main(int argc, char **argv) {
  char buffer[BUFFER + 1], *cursor, *separator;
  int sock, socksize = 1 << 21;
  ssize_t length;

  struct sockaddr_nl addr = {
    .nl_family = AF_NETLINK,
    .nl_groups = 1
  };

  if ((sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT)) < 0)
    err(EXIT_FAILURE, "socket");

  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &socksize, sizeof(int));
  setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &socksize, sizeof(int));

  if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    err(EXIT_FAILURE, "bind");

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

  return EXIT_FAILURE;
}
