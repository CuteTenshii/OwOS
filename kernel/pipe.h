/* pipe.h — kernel pipe buffers connecting a write end to a read end. */
#ifndef OWOS_PIPE_H
#define OWOS_PIPE_H

#include <stdint.h>

int pipe_alloc(void);                         /* new pipe (1 reader, 1 writer), or -1 */
void pipe_ref(int id, int writer);            /* fork inherited an end               */
void pipe_close(int id, int writer);          /* an end was closed                   */
int pipe_read(int id, char *buf, int len);    /* blocks; 0 = EOF              */
int pipe_read_nb(int id, char *buf, int len); /* >0 / 0 would-block / -1 EOF  */
int pipe_write(int id, const char *buf, int len);

/* Terminal state of a pty, kept on its slave-to-master pipe. */
struct pipe_tty {
  uint16_t cols, rows;
  int fg_pid; /* foreground job, 0 = the shell itself */
};
struct pipe_tty *pipe_tty(int id);

#endif /* OWOS_PIPE_H */
