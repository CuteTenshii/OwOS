/* pipe.h — kernel pipe buffers connecting a write end to a read end. */
#ifndef OWOS_PIPE_H
#define OWOS_PIPE_H

int  pipe_alloc(void);                 /* new pipe (1 reader, 1 writer), or -1 */
void pipe_ref(int id, int writer);     /* fork inherited an end               */
void pipe_close(int id, int writer);   /* an end was closed                   */
int  pipe_read(int id, char *buf, int len);   /* blocks; 0 = EOF              */
int  pipe_write(int id, const char *buf, int len);

#endif /* OWOS_PIPE_H */
