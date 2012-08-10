#include <errno.h>
#include <error.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <sys/socket.h>

#define BUFFER 4096

static pid_t pid;

void handler(int sig) {
  if (pid > 0)
    kill(pid, sig);
  if (sig == SIGTERM)
    exit(EXIT_SUCCESS);
  return;
}

void subprocess(char **argv) {
  int eventpipe[2];
  struct sigaction action;

  if (pipe(eventpipe) < 0)
    error(EXIT_FAILURE, errno, "pipe");

  signal(SIGCHLD, SIG_IGN);
  switch (pid = fork()) {
    case -1:
      error(EXIT_FAILURE, errno, "fork");
    case 0:
      if (dup2(eventpipe[0], STDIN_FILENO) < 0)
        error(EXIT_FAILURE, errno, "dup2");
      close(eventpipe[0]);
      close(eventpipe[1]);
      execvp(argv[0], argv);
      error(EXIT_FAILURE, errno, "exec");
  }

  if (dup2(eventpipe[1], STDOUT_FILENO) < 0)
    error(EXIT_FAILURE, errno, "dup2");
  close(eventpipe[0]);
  close(eventpipe[1]);

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

int main(int argc, char **argv) {
  char buffer[BUFFER + 1], *cursor, *separator;
  int sock;
  ssize_t length;
  struct sockaddr_nl addr;

  if (argc > 1)
    subprocess(argv + 1);

  addr.nl_family = AF_NETLINK;
  addr.nl_pid = getpid();
  addr.nl_groups = 1;

  if ((sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT)) < 0)
    error(EXIT_FAILURE, errno, "socket");

  if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    error(EXIT_FAILURE, errno, "bind");

  while (1) {
    if ((length = recv(sock, &buffer, sizeof(buffer) - 1, 0)) < 0) {
      if (errno != EAGAIN && errno != EINTR)
        error(EXIT_FAILURE, errno, "recv");
      continue;
    }

    buffer[length] = 0;
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
