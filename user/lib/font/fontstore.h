/* fontstore.h, the installed-font store under /etc/fonts */
#ifndef OWOS_FONTSTORE_H
#define OWOS_FONTSTORE_H

/* FONT_EMOJI and FONT_CJK are fallbacks for codepoints the text fonts lack: colour emoji, and Han, kana and Hangul.
 * Each installed face serves one role, read from the file: colour bitmaps make it emoji, OS/2 CJK coverage CJK, fixed
 * pitch mono, anything else sans. */
enum { FONT_SANS, FONT_MONO, FONT_EMOJI, FONT_CJK, FONT_ROLES };

/* Validate the .ttf/.otf/.woff/.woff2 file at `src` and copy it into /etc/fonts (web fonts are
 * unpacked to .ttf/.otf; .woff/.woff2 need woff.o linked). `name` (>= 64 bytes) gets the installed
 * file name. Returns 0, or -1 if unreadable, not a usable font, or the write failed. */
int fontstore_install(const char *src, char *name);

/* File names of the installed sans and mono faces, by name: the i-th into `name` (>= 64 bytes); -1 past the end.
 * i == 0 rereads /etc/fonts. */
int fontstore_list(int i, char *name);

/* Path of the font used for `role`: the choice in /etc/fonts.conf if installed, else the upright regular face of the
 * installed family with the most faces in that role. Sans and mono stand in for each other; with no font for the
 * role the path is the /etc/fonts directory itself, which no font loader accepts. */
void fontstore_path(int role, char *path, int n);

/* File name of the font used for `role` (>= 64 bytes). */
void fontstore_current(int role, char *name);

/* Persist `name` as the font for `role`. Returns 0 or -1. */
int fontstore_choose(int role, const char *name);

/* Path of an installed font whose family is `family` (case-insensitive); 0 if none, else 1. */
int fontstore_find(const char *family, char *path, int n);

/* Path of the installed face of `family` (case-insensitive) nearest to `weight` (100..900) and `italic`, preferring
 * the right slant over the right weight. got_weight/got_italic (either may be 0) report the face found. Returns 1, or
 * 0 if no face of that family is installed. */
int fontstore_match(const char *family, int weight, int italic, char *path, int n, int *got_weight, int *got_italic);

#endif
