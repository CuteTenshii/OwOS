/* shell.h — the serial/text recovery console.
 *
 * The desktop terminal is now the /bin/term, which does its own line
 * editing and command dispatch. This kernel shell remains only as the console
 * on the bare text screen / COM1 (`make debug`): the main loop feeds it one
 * character at a time (shell_feed) and it runs the same builtin table.
 */
#ifndef OWOS_SHELL_H
#define OWOS_SHELL_H

void shell_init(void);      /* print the banner and first prompt            */
void shell_feed(char c);    /* process one input character                  */
void shell_interrupt(void); /* Ctrl+C at the prompt: cancel the current line */

#endif /* OWOS_SHELL_H */
