/* users.h — multi-user identity resolution over /etc/passwd.
 *
 * Credentials are PER PROCESS: every task carries a uid/gid (task_t.uid/gid in
 * sched.c, inherited across spawn/fork exactly like cwd), so two processes owned
 * by different users report different identities. `whoami` therefore reflects
 * the *calling* process, not a single global session.
 *
 * This module maps between a uid and the human-readable account, and performs
 * the one privileged operation user space must not be able to fake: validating a
 * password before switching the caller's credentials. It is exposed through
 * /proc:
 *
 *   /proc/whoami  (read)   -> the calling task's user name
 *   /proc/user    (read)   -> "uid gid name home" for the calling task
 *   /proc/login   (write)  -> "name" or "name password": switch the *caller's*
 *                             credentials, validated against /etc/passwd
 *
 * The account *database* lives in ordinary owofs files — /etc/passwd and
 * /etc/group — created and managed entirely in user space (the shell's
 * useradd/groupadd/passwd/... commands). The kernel only owns live credentials
 * and the trusted password check.
 *
 * /etc/passwd line:  name:pwhash:uid:gid:home
 *   pwhash is empty for a passwordless account, else the lowercase-hex SHA-256
 *   of the password. The kernel hashes the *plaintext* it receives and compares,
 *   so a process that can read the stored hash still cannot replay it.
 */
#ifndef OWOS_USERS_H
#define OWOS_USERS_H

#include <stdint.h>

#define USER_NAME_MAX 32
#define USER_HOME_MAX 64

/* Boot-time bring-up: nothing to do beyond confirming the defaults (the main
 * task is already uid 0). Present so kernel_main can call it after fs_init. */
void users_init(void);

/* Resolve a uid to its account name / home directory via /etc/passwd, into
 * caller-supplied storage. Falls back to "root"/"/home/root" for uid 0 and
 * "user"/"/home/user" otherwise when the database has no matching line. */
void users_name_of(uint32_t uid, char *buf, int max);
void users_home_of(uint32_t uid, char *buf, int max);

/* Switch the *calling task's* credentials to `name`. The account must exist in
 * /etc/passwd; if it has a password set, `pass` (plaintext, may be NULL/"") must
 * match. Returns 0 on success, -1 unknown user, -2 wrong password. */
int users_login(const char *name, const char *pass);

/* Validate name/pass against /etc/passwd WITHOUT changing the caller's
 * credentials, reporting the account's uid/gid/home (home may be NULL). For the
 * SSH server: it authenticates a remote login from a kernel thread, then spawns
 * the user's shell with these creds. 0 ok, -1 unknown user, -2 wrong password. */
int users_authenticate(const char *name, const char *pass,
                       uint32_t *uid, uint32_t *gid, char *home, int homesz);

#endif /* OWOS_USERS_H */
