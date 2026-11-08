/* sh.h: what the shell's modules share (see sh.c for the overview). */
#ifndef OWOS_SH_H
#define OWOS_SH_H

#include "clipboard.h"
#include "crypto.h" /* SHA-256 for passwd hashing (su/login/passwd) */
#include "deflate.h"
#include "idnames.h"
#include "lzss.h"
#include "modeutil.h"
#include "owos.h"
#include <stdbool.h>

/* Output colours, as ANSI SGR codes (0: the terminal's default) */
#define FG 0
#define FG_PROMPT 32
#define FG_DIR 94
#define FG_ERR 31

/* out.c */
void oflush(void);
void tputc(char c);
void tputs(const char *s);
void tdec(unsigned v);
void tputs_c(const char *s, unsigned color);
void err(const char *a, const char *b);
void errc(int rc, const char *cmd, const char *msg, const char *path);

/* vars.c */
extern char username[32];
extern char home[256];
extern char hostnm[64];
extern int last_status; /* exit status of the last program, for $? */
void reload_identity(void);
bool has_prefix(const char *s, const char *p);
extern char cwd[256];
void abspath(const char *name, char *out);
void sync_env(void);
bool valid_var_name(const char *s, int len);
bool is_assignment(const char *w);
void set_assignment(const char *w);
unsigned to_uint(const char *s);
bool find_command(const char *name, char *out, int cap);

/* jobs.c */
extern int last_bg_pid; /* for $! */
void bg_service(void);
extern const char *current_line;
void run_other(int argc, char **argv, bool background);
void bi_jobs(void);
void bi_fg(int argc, char **argv);
void bi_wait(int argc, char **argv);
void bi_kill(int argc, char **argv);

/* files.c */
void bi_ls(int argc, char **argv);
void bi_cat(int argc, char **argv);
void bi_cp(int argc, char **argv);
void bi_stat(int argc, char **argv);
void bi_umask(int argc, char **argv);
int cat_proc(const char *name);
void bi_uptime(void);
void bi_which(int argc, char **argv);
void bi_loadkeys(int argc, char **argv);
void bi_clip(int argc, char **argv);
void bi_mount(int argc, char **argv);
void bi_umount(int argc, char **argv);
void bi_services(int argc, char **argv);
void do_compress(int argc, char **argv, const char *suffix, const char *cmd,
                 int (*comp)(const uint8_t *, uint32_t, uint8_t *, uint32_t), uint32_t (*bound)(uint32_t));
void do_decompress(int argc, char **argv, const char *suffix, const char *cmd,
                   int (*dec)(const uint8_t *, uint32_t, uint8_t *, uint32_t), int (*orig)(const uint8_t *, uint32_t));
void bi_grep(int argc, char **argv);

/* accounts.c */
void cmd_useradd(int argc, char **argv);
void cmd_userdel(int argc, char **argv);
void cmd_passwd(int argc, char **argv);
void cmd_groupadd(int argc, char **argv);
void cmd_groupdel(int argc, char **argv);
void cmd_usermod(int argc, char **argv);
void cmd_users(void);
void cmd_groups(int argc, char **argv);
void cmd_id(int argc, char **argv);
void do_login(const char *name, const char *pass);
void cmd_su(int argc, char **argv);
void cmd_login(int argc, char **argv);
void bi_whoami(void);
void bi_hostname(int argc, char **argv);

/* parse.c */
void run_line(const char *text);

/* edit.c */
void prompt(void);
extern char *line;
void history_add(void);
bool edit_line(void);

/* sh.c */
bool is_builtin(const char *name);
void run_argv(int argc, char **argv);

#endif
