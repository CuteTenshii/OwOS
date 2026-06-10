/* file.h — per-process file descriptors. */
#ifndef OWOS_FILE_H
#define OWOS_FILE_H

#include <stdint.h>

#define MAX_FD 8

enum {
	FD_NONE = 0,
	FD_CONSOLE,    /* keyboard (read) / VGA (write)                          */
	FD_PIPE_RD,    /* read end of a pipe                                     */
	FD_PIPE_WR,    /* write end of a pipe                                    */
};

struct fd {
	uint8_t type;
	uint8_t pipe;  /* index into the pipe pool, when type is a pipe end      */
};

#endif /* OWOS_FILE_H */
