#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/kexec.h>
#include <sys/syscall.h>

static int usage(char *progname) {
  fprintf(stderr, "\
Usage: %s [OPTIONS]\n\
Options:\n\
  -c CMDLINE  set kernel command line\n\
  -k FILE     load kernel image from FILE\n\
  -p          execute this kernel on panic\n\
  -r FILE     load initramfs from FILE\n\
", progname);
  return 64;
}

int main(int argc, char **argv) {
  unsigned long flags = KEXEC_FILE_UNLOAD | KEXEC_FILE_NO_INITRAMFS;
  int kernel = -1, initramfs = -1, option;
  char *cmdline = NULL;

  while ((option = getopt(argc, argv, ":c:k:pr:")) > 0)
    switch (option) {
      case 'c':
        cmdline = optarg;
        break;
      case 'k':
        if ((kernel = open(optarg, O_RDONLY)) < 0)
          err(EXIT_FAILURE, "%s", optarg);
        flags &= ~KEXEC_FILE_UNLOAD;
        break;
      case 'p':
        flags |= KEXEC_FILE_ON_CRASH;
        break;
      case 'r':
        if ((initramfs = open(optarg, O_RDONLY)) < 0)
          err(EXIT_FAILURE, "%s", optarg);
        flags &= ~KEXEC_FILE_NO_INITRAMFS;
        break;
      default:
        return usage(argv[0]);
    }

  if (optind != argc)
    return usage(argv[0]);
  if (kernel < 0 && (cmdline || initramfs >= 0))
    errx(EXIT_FAILURE, "Kernel image not specified");

  if (syscall(SYS_kexec_file_load, kernel, initramfs,
        cmdline ? strlen(cmdline) + 1 : 0, cmdline, flags) < 0)
    err(EXIT_FAILURE, "kexec_file_load");
  return EXIT_SUCCESS;
}
