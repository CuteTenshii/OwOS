/* users.c — per-process identity resolution + login (see users.h). */

#include <stddef.h>

#include "users.h"
#include "sched.h"
#include "fs.h"
#include "crypto.h"
#include "kstring.h"

/* ---- tiny local string helpers (kstring.h is intentionally minimal) -------- */

static void cpy(char *d, const char *s, int max)
{
	int i = 0;
	for (; s[i] && i < max - 1; i++) d[i] = s[i];
	d[i] = '\0';
}

/* Parse a non-negative decimal prefix of `s` into *out; return the char after
 * the digits. */
static const char *parse_u32(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
	*out = v;
	return s;
}

/* Lowercase-hex SHA-256 of `pass` into out[65] (64 hex + NUL). */
static void hash_pw(const char *pass, char out[65])
{
	uint8_t dg[SHA256_DIGEST];
	uint32_t n = 0;
	while (pass[n]) n++;
	sha256(pass, n, dg);
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < SHA256_DIGEST; i++) {
		out[i * 2]     = hx[(dg[i] >> 4) & 0xF];
		out[i * 2 + 1] = hx[dg[i] & 0xF];
	}
	out[64] = '\0';
}

/* Copy one ':'-delimited field starting at *p into buf[max]; advance *p past the
 * field (leaving it on the ':' or the line terminator). */
static void take_field(const char **p, char *buf, int max)
{
	const char *s = *p;
	int i = 0;
	while (*s && *s != ':' && *s != '\n' && *s != '\r') {
		if (i < max - 1) buf[i++] = *s;
		s++;
	}
	buf[i] = '\0';
	*p = s;
}

/* Advance `line` to the start of the next /etc/passwd line. */
static const char *next_line(const char *p)
{
	while (*p && *p != '\n') p++;
	while (*p == '\n' || *p == '\r') p++;
	return p;
}

/* One parsed passwd record. */
struct pwent {
	char     name[USER_NAME_MAX];
	char     pwhash[80];
	uint32_t uid, gid;
	char     home[USER_HOME_MAX];
};

/* Parse the passwd line at `line` into *e. Returns the start of the next line. */
static const char *parse_line(const char *line, struct pwent *e)
{
	const char *p = line;
	e->pwhash[0] = e->home[0] = '\0';
	e->uid = e->gid = 0;
	take_field(&p, e->name, sizeof e->name);
	if (*p == ':') { p++; take_field(&p, e->pwhash, sizeof e->pwhash); }
	if (*p == ':') { p++; p = parse_u32(p, &e->uid); }
	if (*p == ':') { p++; p = parse_u32(p, &e->gid); }
	if (*p == ':') { p++; take_field(&p, e->home, sizeof e->home); }
	return next_line(p);
}

/* Look up the passwd record matching either a name (`want_name`, when non-NULL)
 * or a uid (`want_uid`). Returns 1 and fills *out on a hit, 0 if absent. */
static int passwd_lookup(const char *want_name, uint32_t want_uid, struct pwent *out)
{
	static char buf[4096];
	int n = fs_read("/etc/passwd", buf, sizeof buf - 1);
	if (n < 0)
		return 0;
	buf[n] = '\0';
	for (const char *line = buf; *line; ) {
		struct pwent e;
		line = parse_line(line, &e);
		if (!e.name[0])
			continue;
		int hit = want_name ? streq(e.name, want_name) : (e.uid == want_uid);
		if (hit) {
			*out = e;
			return 1;
		}
	}
	return 0;
}

void users_init(void)
{
	/* The boot session is already uid 0 (sched.c initialises the main task to
	 * root). The account database is created later in kernel_main; nothing else
	 * to bring up here. */
}

void users_name_of(uint32_t uid, char *buf, int max)
{
	struct pwent e;
	if (passwd_lookup(NULL, uid, &e))
		cpy(buf, e.name, max);
	else
		cpy(buf, uid == 0 ? "root" : "user", max);
}

void users_home_of(uint32_t uid, char *buf, int max)
{
	struct pwent e;
	if (passwd_lookup(NULL, uid, &e) && e.home[0])
		cpy(buf, e.home, max);
	else
		cpy(buf, uid == 0 ? "/home/root" : "/home/user", max);
}

int users_login(const char *name, const char *pass)
{
	struct pwent e;
	if (!passwd_lookup(name, 0, &e))
		return -1;                         /* unknown user                       */

	if (e.pwhash[0]) {                         /* password required              */
		char given[65];
		hash_pw(pass ? pass : "", given);
		if (!streq(given, e.pwhash))
			return -2;                     /* wrong password                     */
	}

	sched_set_creds_self(e.uid, e.gid);        /* switch the caller's identity   */
	return 0;
}
