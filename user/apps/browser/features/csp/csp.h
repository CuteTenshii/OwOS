/* csp.h: Content-Security-Policy engine. A resource must satisfy every added policy.
 * frame-ancestors is not handled here (checked by the embedder in resource.c). */
#ifndef OWOS_BROWSER_CSP_H
#define OWOS_BROWSER_CSP_H

typedef struct csp csp;

/* Resource kinds the directives govern; CSP_CHILD is internal (child-src fallback for frames). */
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

/* Check a load of `type`; `out` gets the URL to use (upgraded if upgrade-insecure-requests). 1 = allowed. */
int csp_check_url(const csp *c, int type, const char *abs_url, const char *page_url, char *out, int out_cap,
                  char *reason, int rcap);

/* Check an external <script src> against script-src, honouring nonce and 'strict-dynamic'. 1 = allowed. */
int csp_check_script_el(const csp *c, const char *abs_url, const char *page_url, const char *nonce, int parser_inserted,
                        char *out, int out_cap, char *reason, int rcap);

/* Check an inline CSP_SCRIPT/CSP_STYLE by nonce or sha256 of body. 1 = allowed. */
int csp_check_inline(const csp *c, int type, const char *nonce, const unsigned char *body, int len, char *reason,
                     int rcap);

int csp_allows_eval(const csp *c); /* is dynamic code generation permitted? */

#endif /* OWOS_BROWSER_CSP_H */
