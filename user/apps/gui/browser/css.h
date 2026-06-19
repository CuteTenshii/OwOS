/* css.h — a small but real CSS engine: parse <style> blocks / style="" into
 * rules, match selectors against DOM nodes, cascade by specificity, and produce
 * a computed style. The renderer (html.c) maps the result onto its capabilities
 * (16-colour text, bold/underline, block/inline, image box size). */
#ifndef OWOS_CSS_H
#define OWOS_CSS_H

#include "dom.h"

enum { CSS_DISP_UNSET = 0, CSS_DISP_INLINE, CSS_DISP_BLOCK, CSS_DISP_NONE };

/* A computed-style delta. Each field is either set or "unset" so deltas layer:
 * colours are 0xRRGGBB or -1; bold/under are 0/1 or -1; width/height are px or
 * -1; disp is a CSS_DISP_* value (UNSET = 0); fam is the full font-family stack
 * (each name lowercased + dequoted, comma-joined), "" = unset. */
#define CSS_FAM_MAX   48     /* one family name (e.g. an @font-face's font-family) */
#define CSS_FAM_STACK 160    /* a whole font-family stack: "A", "B", sans-serif    */
struct css_style {
	int          color;
	int          bg;
	signed char  bold;
	signed char  under;
	signed char  disp;
	signed char  center;   /* text-align: 1 = center, 0 = left/other, -1 unset */
	int          width, height;
	char         fam[CSS_FAM_STACK];
};

typedef struct css_sheet css_sheet;

void css_style_reset(struct css_style *s);

css_sheet *css_new(void);
void       css_add(css_sheet *sheet, const char *src, int len);   /* a <style> block */
void       css_free(css_sheet *sheet);

/* The current viewport, what @media (min/max-width|height), orientation and
 * prefers-color-scheme test against. The browser sets it before each cascade
 * (and on resize, so media queries re-evaluate). Defaults to a sane size. */
void       css_set_viewport(int width, int height);

/* @font-face faces declared in the sheet: each a family name + a src URL (the
 * first raw TTF/OTF url; woff/woff2 entries are skipped — we can't decode them).
 * The browser fetches the url and loads the face, keyed by family. */
int  css_fontface_count(const css_sheet *sheet);
void css_fontface_at(const css_sheet *sheet, int i, const char **family, const char **url);

/* Fold the sheet's matching rules for `el` into *out (specificity/order order). */
void css_apply_sheet(css_sheet *sheet, const struct dom_node *el, struct css_style *out);
/* Fold a style="" declaration block into *out (highest priority). */
void css_apply_inline(const char *decls, struct css_style *out);
/* Fold HTML presentational attrs (color/bgcolor/width/height) into *out (lowest). */
void css_apply_attrs(const struct dom_node *el, struct css_style *out);

/* Devtools Styles pane: the rules whose selector matches `el`, winner first.
 * Each carries the reconstructed selector text, the @media prelude ("" if none)
 * plus whether that media currently applies, and the rule's declarations (the
 * subset css.c models). Returns the number written (<= cap). */
struct css_match {
	char             selector[160];
	char             media[128];
	int              active;        /* 1 = the @media currently matches */
	struct css_style decl;
};
int css_matches(css_sheet *sheet, const struct dom_node *el,
                struct css_match *out, int cap);

#endif /* OWOS_CSS_H */
