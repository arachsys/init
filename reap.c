#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
  signal(SIGCHLD, SIG_IGN);
  while (1)
    pause();
  return EXIT_SUCCESS;
}
