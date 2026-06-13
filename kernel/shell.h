/* shell.h — a tiny line-buffered command shell.
 *
 * The main loop feeds it one character at a time (shell_feed). It echoes
 * input, handles backspace, and on Enter parses and runs a command.
 */
#ifndef OWOS_SHELL_H
#define OWOS_SHELL_H

void shell_init(void);       /* print the banner and first prompt            */
void shell_feed(char c);     /* process one input character                  */
void shell_interrupt(void);  /* Ctrl+C at the prompt: cancel the current line */

/* Run a builtin command with its console output captured into buf, for the
 * ring-3 terminal's bridge syscall. Returns bytes captured, or -1 if the first
 * token isn't a builtin. */
int  shell_exec_capture(const char *cmd, char *buf, int max);

/* Streaming variant of the bridge: run a command in a persistent producer
 * thread and drain its output in chunks, so a long/voluminous command renders
 * as it runs instead of freezing the terminal until it finishes.
 *   shell_stream_start  -- create the runner thread once (at boot).
 *   shell_stream_begin  -- start `cmd`; 0 if streamable, -1 if not (caller falls
 *                          back to launching it as a detached GUI app).
 *   shell_stream_read   -- drain up to max bytes: >0 count, 0 running-but-idle,
 *                          -1 end of output. */
void shell_stream_start(void);
int  shell_stream_begin(const char *cmd);
int  shell_stream_read(char *buf, int max);
int  shell_stream_active(void);   /* a streamed command is running              */
void shell_stream_interrupt(void); /* Ctrl+C: echo ^C, deliver cooperative intr */
void shell_stream_abort(void);    /* close: hard-kill a CLI child (no echo)      */

#endif /* OWOS_SHELL_H */
