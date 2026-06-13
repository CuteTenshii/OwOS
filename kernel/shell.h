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

#endif /* OWOS_SHELL_H */
