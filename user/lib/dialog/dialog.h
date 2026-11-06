/* dialog.h — the reusable modal message box for windowed apps.
 *
 * A message box is the one dialog worth standardizing in a lib: an icon, a line
 * of text, and a standard button set (OK / Cancel / Yes / No). It is MODAL — it
 * blocks (running its own event loop) because the app genuinely cannot proceed
 * until the user answers — and returns which button was pressed.
 *
 * Richer dialogs (a Properties sheet, a Find bar, a settings form) are NOT a
 * lib concern: their content is app-specific, and they are usually MODELESS
 * (the app keeps running). An app builds those itself as a transient child
 * window — win_create() + win_set_parent(id, parent, 0) for a non-blocking
 * child, then win_fb_of(id) for its pixel buffer — and draws + pumps it from
 * its own loop. See user/apps/files/files.c (the Properties window) for the
 * pattern.
 */
#ifndef OWOS_DIALOG_H
#define OWOS_DIALOG_H

/* Message-box icon — pick one (or DLG_ICON_NONE). */
#define DLG_ICON_NONE 0x00
#define DLG_ICON_INFO 0x01
#define DLG_ICON_WARN 0x02
#define DLG_ICON_ERROR 0x03
#define DLG_ICON_QUESTION 0x04
#define DLG_ICON_MASK 0x07

/* Message-box button set — pick one. The default (Enter) button is the primary
 * affirmative (rightmost); Esc / the close box choose the safe/negative one. */
#define DLG_OK 0x00          /* [ OK ]                    (default) */
#define DLG_OKCANCEL 0x10    /* [ Cancel ] [ OK ]                   */
#define DLG_YESNO 0x20       /* [ No ] [ Yes ]                      */
#define DLG_YESNOCANCEL 0x30 /* [ Cancel ] [ No ] [ Yes ]          */
#define DLG_BTN_MASK 0x30

/* Which button dismissed the box. */
enum dlg_result { DLG_R_CLOSE = 0, DLG_R_OK, DLG_R_CANCEL, DLG_R_YES, DLG_R_NO };

/* Show a modal message box as a transient child of window `parent` (the
 * caller's own window id; pass -1 for a parentless top-level dialog). Blocks
 * until a button is pressed (or Esc / the close box dismisses it), then returns
 * the dlg_result and restores the caller's gfx surface. `flags` = one
 * DLG_ICON_* | one button set. */
int dialog_msg(int parent, const char *title, const char *message, unsigned flags);

/* Convenience wrappers. dialog_message shows an info box with a single OK;
 * dialog_confirm asks a Yes/No question and returns 1 for Yes, 0 otherwise. */
int dialog_message(int parent, const char *title, const char *message);
int dialog_confirm(int parent, const char *title, const char *message);

/* Modal dialog with a message and 2..4 custom-labelled buttons (right-aligned,
 * the last = primary/default fired by Enter). Blocks like dialog_msg and
 * returns the 0-based index of the chosen button, or -1 if dismissed (Esc /
 * close box). `iconflags` is one DLG_ICON_* (button-set bits are ignored — the
 * labels define the buttons). Where the safe choice is a specific button, treat
 * -1 as that one. Parented on a window this is modal (win_set_parent), so the
 * compositor draws no close/min/max chrome and it can't be dragged — the user
 * must answer its buttons. */
int dialog_choose(int parent, const char *title, const char *message, const char *const *labels, int nlabels,
                  unsigned iconflags);

/* Modal single-line text-input dialog: an optional message, an input pre-filled
 * with `initial`, and OK/Cancel. On OK, copies the entered text into out[outsz]
 * and returns 1; on Cancel/Esc/close returns 0 and leaves out unchanged. This
 * is the host side of JS window.prompt(). parent as in dialog_msg. */
int dialog_prompt(int parent, const char *title, const char *message, const char *initial, char *out, int outsz);

#endif /* OWOS_DIALOG_H */
