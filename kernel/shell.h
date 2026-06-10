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

#endif /* OWOS_SHELL_H */
