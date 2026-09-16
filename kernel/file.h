/* file.h — per-process file descriptors. */
#ifndef OWOS_FILE_H
#define OWOS_FILE_H

#include <stdint.h>

#define MAX_FD 8

enum {
  FD_NONE = 0,
  FD_CONSOLE, /* keyboard (read) / VGA (write)                          */
  FD_PIPE_RD, /* read end of a pipe                                     */
  FD_PIPE_WR, /* write end of a pipe                                    */
  FD_PORT,    /* an IPC listen handle (server side); `pipe` = port id   */
  FD_CHANNEL, /* a bidirectional IPC connection: read `pipe`, write `tx`*/
};

struct fd {
  uint8_t type;
  uint8_t pipe; /* a pipe-pool index (pipe end / channel rx), or a port id */
  uint8_t tx;   /* channel tx pipe-pool index, when type is FD_CHANNEL     */
};

#endif /* OWOS_FILE_H */
