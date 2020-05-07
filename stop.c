#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <sys/reboot.h>

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "halt"))
    return reboot(RB_HALT_SYSTEM);
  else if (argc == 2 && !strcmp(argv[1], "kexec"))
    return reboot(RB_KEXEC);
  else if (argc == 2 && !strcmp(argv[1], "poweroff"))
    return reboot(RB_POWER_OFF);
  else if (argc == 2 && !strcmp(argv[1], "reboot"))
    return reboot(RB_AUTOBOOT);
  else if (argc == 2 && !strcmp(argv[1], "suspend"))
    return reboot(RB_SW_SUSPEND);

  fprintf(stderr, "\
Usage: %s ACTION\n\
Actions:\n\
  halt      halt the machine\n\
  kexec     jump to a new kernel loaded for kexec\n\
  poweroff  switch off the machine\n\
  reboot    restart the machine\n\
  suspend   hibernate the machine to disk\n\
All actions are performed immediately without flushing buffers or a\n\
graceful shutdown. Data may be lost on unsynced mounted filesystems.\n\
", argv[0]);
  return EX_USAGE;
}
