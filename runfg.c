#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
  int result = EXIT_FAILURE, status;
  pid_t child, command;

  if (argc < 2) {
    fprintf(stderr, "Usage: %s CMD [ARG]...\n", argv[0]);
    return 64;
  }

  if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) < 0)
    err(EXIT_FAILURE, "prctl PR_SET_CHILD_SUBREAPER");

  switch (command = fork()) {
    case -1:
      err(EXIT_FAILURE, "fork");
    case 0:
      execvp(argv[1], argv + 1);
      err(EXIT_FAILURE, "exec %s", argv[1]);
  }

  while (1) {
    child = waitpid(-1, &status, 0);
    if (child < 0) {
      if (errno == ECHILD)
        return result;
      if (errno != EINTR)
        err(EXIT_FAILURE, "waitpid");
    } else if (child == command) {
      if (WIFEXITED(status))
        result = WEXITSTATUS(status);
      if (WIFSIGNALED(status))
        result = 128 + WTERMSIG(status);
      command = -1;
    }
  }
}
