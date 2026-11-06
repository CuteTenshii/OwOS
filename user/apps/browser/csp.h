/* csp.h — Content-Security-Policy engine for the browser.
 *
 * A page may ship several CSP policies (one per Content-Security-Policy header,
 * plus any <meta http-equiv>); a resource must satisfy EVERY policy. csp_add()
 * folds one policy text into the set; the check functions answer whether a
 * given load is permitted, applying the directive fallback chain (a fetch
 * directive falls back to default-src; frame-src falls back to child-src then
 * default-src) and the full source-expression grammar: 'none' / 'self' /
 * 'unsafe-inline' / 'unsafe-eval' / 'nonce-...' / 'sha256-...' / scheme sources
 * (https:, data:) / host sources with wildcards, ports and paths
 * (*.x.com:443/p) / '*'.
 *
 * frame-ancestors is NOT handled here: it is the *embedder's* check on a framed
 * response, done where the iframe is fetched (resource.c). This module covers
 * the directives that constrain what the current document itself may load and
 * run.
 */
#ifndef OWOS_BROWSER_CSP_H
#define OWOS_BROWSER_CSP_H

typedef struct csp csp;

/* Resource/destination kinds the directives govern. CSP_CHILD is internal (the
 * child-src fallback for frames); callers pass the others. */
enum {
  CSP_DEFAULT,
  CSP_SCRIPT,
  CSP_STYLE,
  CSP_IMG,
  CSP_FONT,
  CSP_CONNECT,
  CSP_FRAME,
  CSP_CHILD,
  CSP_FORM,
  CSP_MEDIA,
  CSP_OBJECT,
  CSP_NTYPES
};

csp *csp_new(void);
void csp_add(csp *c, const char *policy); /* fold in one policy (header/meta) */
void csp_free(csp *c);
int csp_empty(const csp *c); /* 1 = nothing to enforce          */

/* Check a resource load of `type` for the absolute URL `abs_url`. The URL to
 * actually use is written to `out` (http->https upgraded when a policy sets
 * upgrade-insecure-requests). Returns 1 = allowed, 0 = blocked (with a short
 * human reason in `reason`). */
int csp_check_url(const csp *c, int type, const char *abs_url, const char *page_url, char *out, int out_cap,
                  char *reason, int rcap);

/* Check an external <script src> element against script-src. Beyond the
 * host-source match csp_check_url does, this honours the element's `nonce` and
 * 'strict-dynamic' (which trusts scripts injected by an already-run script —
 * `parser_inserted` is 0 for those, 1 for scripts that came straight from the
 * HTML parser). `out` receives the URL to use (upgrade rewrite applied).
 * Returns 1 = allowed. */
int csp_check_script_el(const csp *c, const char *abs_url, const char *page_url, const char *nonce, int parser_inserted,
                        char *out, int out_cap, char *reason, int rcap);

/* Check an inline <script>/<style> (type CSP_SCRIPT or CSP_STYLE). `nonce` is
 * the element's nonce attribute (or NULL); `body`/`len` is the inline source,
 * hashed for 'sha256-...' matching. Returns 1 = allowed. */
int csp_check_inline(const csp *c, int type, const char *nonce, const unsigned char *body, int len, char *reason,
                     int rcap);

int csp_allows_eval(const csp *c); /* is dynamic code generation permitted? */

#endif /* OWOS_BROWSER_CSP_H */
