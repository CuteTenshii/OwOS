/* dialog.h, the reusable modal message box for windowed apps */
#ifndef OWOS_DIALOG_H
#define OWOS_DIALOG_H

/* Message-box icon, pick one (or DLG_ICON_NONE) */
#define DLG_ICON_NONE 0x00
#define DLG_ICON_INFO 0x01
#define DLG_ICON_WARN 0x02
#define DLG_ICON_ERROR 0x03
#define DLG_ICON_QUESTION 0x04
#define DLG_ICON_MASK 0x07

/* Message-box button set, pick one */
#define DLG_OK 0x00          /* [ OK ] (default) */
#define DLG_OKCANCEL 0x10    /* [ Cancel ] [ OK ] */
#define DLG_YESNO 0x20       /* [ No ] [ Yes ] */
#define DLG_YESNOCANCEL 0x30 /* [ Cancel ] [ No ] [ Yes ] */
#define DLG_BTN_MASK 0x30

/* Which button dismissed the box */
enum dlg_result { DLG_R_CLOSE = 0, DLG_R_OK, DLG_R_CANCEL, DLG_R_YES, DLG_R_NO };

/* Show a modal message box as a transient child of window `parent` */
int dialog_msg(int parent, const char *title, const char *message, unsigned flags);

/* Convenience wrappers */
int dialog_message(int parent, const char *title, const char *message);
int dialog_confirm(int parent, const char *title, const char *message);

/* Modal dialog with a message and 2..4 custom-labelled buttons */
int dialog_choose(int parent, const char *title, const char *message, const char *const *labels, int nlabels,
                  unsigned iconflags);

/* Modal single-line text-input dialog: an optional message, an input pre-filled with `initial` */
int dialog_prompt(int parent, const char *title, const char *message, const char *initial, char *out, int outsz);

#endif /* OWOS_DIALOG_H */
