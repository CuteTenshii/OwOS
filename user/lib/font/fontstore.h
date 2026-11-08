/* fontstore.h, the installed-font store under /etc/fonts */
#ifndef OWOS_FONTSTORE_H
#define OWOS_FONTSTORE_H

enum { FONT_SANS, FONT_MONO };

/* Validate the .ttf/.otf/.woff/.woff2 file at `src` and copy it into /etc/fonts (web fonts are
 * unpacked to .ttf/.otf; .woff/.woff2 need woff.o linked). `name` (>= 64 bytes) gets the installed
 * file name. Returns 0, or -1 if unreadable, not a usable font, or the write failed. */
int fontstore_install(const char *src, char *name);

/* Installed font file names: the i-th entry into `name` (>= 64 bytes); -1 past the end. */
int fontstore_list(int i, char *name);

/* Path of the font used for `role`: the choice in /etc/fonts.conf if installed, else the built-in. */
void fontstore_path(int role, char *path, int n);

/* File name of the font used for `role` (>= 64 bytes). */
void fontstore_current(int role, char *name);

/* Persist `name` as the font for `role`. Returns 0 or -1. */
int fontstore_choose(int role, const char *name);

/* Path of an installed font whose family is `family` (case-insensitive); 0 if none, else 1. */
int fontstore_find(const char *family, char *path, int n);

#endif
