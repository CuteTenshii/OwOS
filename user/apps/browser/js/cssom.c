/* CSSOM: element.style and CSS.supports(). */

#include "bindings.h"

/* ------------------------------------------------------- style (CSSOM-lite) - */

static bool is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static bool span_ci_eq(const char *s, const char *e, const char *lit)
{
  while (s < e && *lit) {
    if (lower(*s) != lower(*lit))
      return false;
    s++;
    lit++;
  }
  return s == e && !*lit;
}

static js_value style_read(js_ctx *ctx, struct dom_node *node, const char *prop)
{
  const char *style = node ? dom_get_attr(node, "style") : 0;
  const char *p = style;
  while (p && *p) {
    const char *seg = p, *colon;
    while (*p && *p != ';')
      p++;
    colon = seg;
    while (colon < p && *colon != ':')
      colon++;
    if (colon < p) {
      const char *ns = seg, *ne = colon, *vs = colon + 1, *ve = p;
      while (ns < ne && is_sp(*ns))
        ns++;
      while (ne > ns && is_sp(ne[-1]))
        ne--;
      while (vs < ve && is_sp(*vs))
        vs++;
      while (ve > vs && is_sp(ve[-1]))
        ve--;
      if (ns < ne && span_ci_eq(ns, ne, prop))
        return js_mk_string(ctx, vs, (int)(ve - vs));
    }
    if (*p == ';')
      p++;
  }
  return js_mk_cstring(ctx, "");
}

/* Rewrite the element's style="" with `setprop` set to `setval` (NULL/"" removes it) */
static void style_rebuild(struct dom_node *node, const char *setprop, const char *setval)
{
  const char *style;
  sbuf sb;
  if (!node)
    return;
  sb.b = 0;
  sb.n = 0;
  sb.cap = 0;
  style = dom_get_attr(node, "style");
  {
    const char *p = style;
    while (p && *p) {
      const char *seg = p, *colon;
      while (*p && *p != ';')
        p++;
      colon = seg;
      while (colon < p && *colon != ':')
        colon++;
      if (colon < p) {
        const char *ns = seg, *ne = colon, *vs = colon + 1, *ve = p;
        while (ns < ne && is_sp(*ns))
          ns++;
        while (ne > ns && is_sp(ne[-1]))
          ne--;
        while (vs < ve && is_sp(*vs))
          vs++;
        while (ve > vs && is_sp(ve[-1]))
          ve--;
        if (ns < ne && !span_ci_eq(ns, ne, setprop)) {
          if (sb.n)
            sb_puts(&sb, "; ");
          sb_putn(&sb, ns, (int)(ne - ns));
          sb_puts(&sb, ": ");
          sb_putn(&sb, vs, (int)(ve - vs));
        }
      }
      if (*p == ';')
        p++;
    }
  }
  if (setprop && setval && *setval) {
    if (sb.n)
      sb_puts(&sb, "; ");
    sb_puts(&sb, setprop);
    sb_puts(&sb, ": ");
    sb_puts(&sb, setval);
  }
  dom_set_attr(node, "style", sb.b ? sb.b : "");
  if (sb.b)
    free(sb.b);
}
static void style_write(struct dom_node *node, const char *prop, const char *val) { style_rebuild(node, prop, val); }

#define STYLE_PROP(JSN, CSSN)                                                                                          \
  static js_value style_get_##JSN(js_ctx *ctx, js_value t, js_value *args, int argc)                                   \
  {                                                                                                                    \
    (void)args;                                                                                                        \
    (void)argc;                                                                                                        \
    return style_read(ctx, node_of(t), CSSN);                                                                          \
  }                                                                                                                    \
  static js_value style_set_##JSN(js_ctx *ctx, js_value t, js_value *args, int argc)                                   \
  {                                                                                                                    \
    style_write(node_of(t), CSSN, arg_str(ctx, A(0)));                                                                 \
    return js_undef();                                                                                                 \
  }
STYLE_PROP(color, "color")
STYLE_PROP(background, "background")
STYLE_PROP(backgroundColor, "background-color")
STYLE_PROP(backgroundImage, "background-image")
STYLE_PROP(backgroundPosition, "background-position")
STYLE_PROP(backgroundSize, "background-size")
STYLE_PROP(backgroundRepeat, "background-repeat")
STYLE_PROP(display, "display")
STYLE_PROP(width, "width")
STYLE_PROP(height, "height")
STYLE_PROP(fontWeight, "font-weight")
STYLE_PROP(fontFamily, "font-family")
STYLE_PROP(textAlign, "text-align")
STYLE_PROP(textDecoration, "text-decoration")
#undef STYLE_PROP

js_value style_get_csstext(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  const char *s = n ? dom_get_attr(n, "style") : 0;
  (void)args;
  (void)argc;
  return js_mk_cstring(ctx, s ? s : "");
}
js_value style_set_csstext(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  if (n)
    dom_set_attr(n, "style", arg_str(ctx, A(0)));
  return js_undef();
}
js_value style_setprop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  char name[64];
  const char *ns = arg_str(ctx, A(0));
  int i;
  for (i = 0; ns[i] && i < 63; i++)
    name[i] = ns[i];
  name[i] = '\0';
  style_write(n, name, arg_str(ctx, A(1)));
  return js_undef();
}
js_value style_getprop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  return style_read(ctx, node_of(t), arg_str(ctx, A(0)));
}
js_value style_removeprop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_value old = style_read(ctx, n, arg_str(ctx, A(0)));
  style_rebuild(n, arg_str(ctx, A(0)), 0);
  return old;
}

/* <style>/<link> .sheet: a minimal CSSStyleSheet stub with an empty cssRules list */
js_value el_get_sheet(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_object *sheet;
  js_value rules;
  (void)args;
  (void)argc;
  if (!n || n->type != DOM_ELEMENT || !(streq(n->tag, "style") || streq(n->tag, "link")))
    return js_null();
  gc_push_scope();
  sheet = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  rules = js_obj_value(js_new_array(ctx, 0));
  js_def(sheet, "cssRules", rules, P_ENUMERABLE);
  js_def(sheet, "rules", rules, P_ENUMERABLE);
  gc_pop_scope();
  return js_obj_value(sheet);
}
js_value el_get_style(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *o = js_new_object(ctx, g_style_proto);
  (void)args;
  (void)argc;
  o->host = node_of(t); /* proxy the element's style attr */
  return js_obj_value(o);
}
/* el.dataset: a snapshot object of the element's data-* attributes, keyed in camelCase */
js_value el_get_dataset(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  struct dom_node *n = node_of(t);
  js_object *o;
  struct dom_attr *a;
  (void)args;
  (void)argc;
  gc_push_scope();
  o = (js_object *)gc_keep(js_new_object(ctx, ctx->object_proto));
  if (n)
    for (a = n->attrs; a; a = a->next) {
      if (starts_ci(a->name, "data-") && a->name[5]) {
        char key[64];
        int ki = 0;
        const char *p = a->name + 5;
        while (*p && ki < 63) {
          if (*p == '-' && p[1]) {
            p++;
            key[ki++] = (p[0] >= 'a' && p[0] <= 'z') ? (char)(p[0] - 32) : p[0];
            p++;
          } else
            key[ki++] = *p++;
        }
        key[ki] = '\0';
        js_def(o, key, js_mk_cstring(ctx, a->value ? a->value : ""), P_WRITABLE | P_ENUMERABLE | P_CONFIGURABLE);
      }
    }
  gc_pop_scope();
  return js_obj_value(o);
}
js_value win_getcomputedstyle(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_object *o = js_new_object(ctx, g_style_proto);
  (void)t;
  o->host = node_of(A(0)); /* inline styles only (no cascade) */
  return js_obj_value(o);
}
/* window geometry: the content viewport size and the page's scroll offset */
js_value win_innerWidth(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int w = 0, h = 0;
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  browser_viewport_px(&w, &h);
  return js_number(w);
}
js_value win_innerHeight(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  int w = 0, h = 0;
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  browser_viewport_px(&w, &h);
  return js_number(h);
}
js_value win_scrollX(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(0);
}
js_value win_scrollY(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(browser_scroll_px());
}

/* window.screen, no separate display metric exists */
js_value screen_width(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(BW);
}
js_value screen_height(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_number(BH);
}

js_value el_noop(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_undef();
}
js_value nav_false(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_bool(0);
}
js_value el_true(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)ctx;
  (void)t;
  (void)args;
  (void)argc;
  return js_bool(1);
}
js_value el_empty_array(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  (void)args;
  (void)argc;
  return js_obj_value(js_new_array(ctx, 0));
}

/* --- CSSStyleDeclaration: generic property access backed by the element's */
static js_value style_gen_get(js_ctx *ctx, js_value t, js_value *a, int n)
{
  js_value d = ctx->native_data;
  (void)a;
  (void)n;
  return style_read(ctx, node_of(t), d.type == JV_STR ? d.u.str->data : "");
}
static js_value style_gen_set(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  js_value d = ctx->native_data;
  (void)argc;
  style_write(node_of(t), d.type == JV_STR ? d.u.str->data : "", arg_str(ctx, A(0)));
  return js_undef();
}
js_value style_gen_length(js_ctx *ctx, js_value t, js_value *a, int n)
{
  struct dom_node *nd = node_of(t);
  const char *p = nd ? dom_get_attr(nd, "style") : 0;
  int count = 0;
  (void)ctx;
  (void)a;
  (void)n;
  while (p && *p) {
    const char *seg = p, *colon;
    while (*p && *p != ';')
      p++;
    colon = seg;
    while (colon < p && *colon != ':')
      colon++;
    if (colon < p) { /* count only "name : value" pairs */
      const char *ns = seg, *ne = colon, *vs = colon + 1, *ve = p;
      while (ns < ne && is_sp(*ns))
        ns++;
      while (ne > ns && is_sp(ne[-1]))
        ne--;
      while (vs < ve && is_sp(*vs))
        vs++;
      while (ve > vs && is_sp(ve[-1]))
        ve--;
      if (ns < ne && vs < ve)
        count++;
    }
    if (*p == ';')
      p++;
  }
  return js_number(count);
}
/* camelCase JS property name -> CSS dash-case (cssFloat -> float). */
static void style_js_to_css(const char *js, char *out, int cap)
{
  int o = 0;
  if (streq(js, "cssFloat")) {
    const char *f = "float";
    while (*f && o < cap - 1)
      out[o++] = *f++;
    out[o] = 0;
    return;
  }
  for (int i = 0; js[i] && o < cap - 2; i++) {
    char c = js[i];
    if (c >= 'A' && c <= 'Z') {
      out[o++] = '-';
      out[o++] = (char)(c + 32);
    } else
      out[o++] = c;
  }
  out[o] = 0;
}
void def_style_prop(const char *jsName)
{
  char css[64];
  style_js_to_css(jsName, css, sizeof css);
  gc_push_scope();
  {
    js_object *g = (js_object *)gc_keep(js_new_native(g_ctx, style_gen_get, jsName, 0));
    js_object *s = (js_object *)gc_keep(js_new_native(g_ctx, style_gen_set, jsName, 1));
    g->fn->data = js_mk_cstring(g_ctx, css);
    s->fn->data = js_mk_cstring(g_ctx, css);
    js_def_accessor(g_style_proto, jsName, g, s, P_CONFIGURABLE | P_ENUMERABLE);
  }
  gc_pop_scope();
}
/* The standard CSS property set exposed on CSSStyleDeclaration (camelCase) */
const char *const k_css_props[] = {
    "alignContent",
    "alignItems",
    "alignSelf",
    "all",
    "animation",
    "animationDelay",
    "animationDirection",
    "animationDuration",
    "animationFillMode",
    "animationIterationCount",
    "animationName",
    "animationPlayState",
    "animationTimingFunction",
    "appearance",
    "aspectRatio",
    "backdropFilter",
    "backfaceVisibility",
    "background",
    "backgroundAttachment",
    "backgroundBlendMode",
    "backgroundClip",
    "backgroundColor",
    "backgroundImage",
    "backgroundOrigin",
    "backgroundPosition",
    "backgroundRepeat",
    "backgroundSize",
    "blockSize",
    "border",
    "borderBottom",
    "borderBottomColor",
    "borderBottomLeftRadius",
    "borderBottomRightRadius",
    "borderBottomStyle",
    "borderBottomWidth",
    "borderCollapse",
    "borderColor",
    "borderImage",
    "borderImageOutset",
    "borderImageRepeat",
    "borderImageSlice",
    "borderImageSource",
    "borderImageWidth",
    "borderLeft",
    "borderLeftColor",
    "borderLeftStyle",
    "borderLeftWidth",
    "borderRadius",
    "borderRight",
    "borderRightColor",
    "borderRightStyle",
    "borderRightWidth",
    "borderSpacing",
    "borderStyle",
    "borderTop",
    "borderTopColor",
    "borderTopLeftRadius",
    "borderTopRightRadius",
    "borderTopStyle",
    "borderTopWidth",
    "borderWidth",
    "bottom",
    "boxDecorationBreak",
    "boxShadow",
    "boxSizing",
    "breakAfter",
    "breakBefore",
    "breakInside",
    "captionSide",
    "caretColor",
    "clear",
    "clip",
    "clipPath",
    "color",
    "columnCount",
    "columnFill",
    "columnGap",
    "columnRule",
    "columnRuleColor",
    "columnRuleStyle",
    "columnRuleWidth",
    "columnSpan",
    "columnWidth",
    "columns",
    "content",
    "counterIncrement",
    "counterReset",
    "cursor",
    "direction",
    "display",
    "emptyCells",
    "filter",
    "flex",
    "flexBasis",
    "flexDirection",
    "flexFlow",
    "flexGrow",
    "flexShrink",
    "flexWrap",
    "cssFloat",
    "float",
    "font",
    "fontFamily",
    "fontFeatureSettings",
    "fontKerning",
    "fontSize",
    "fontSizeAdjust",
    "fontStretch",
    "fontStyle",
    "fontVariant",
    "fontWeight",
    "gap",
    "grid",
    "gridArea",
    "gridAutoColumns",
    "gridAutoFlow",
    "gridAutoRows",
    "gridColumn",
    "gridColumnEnd",
    "gridColumnGap",
    "gridColumnStart",
    "gridGap",
    "gridRow",
    "gridRowEnd",
    "gridRowGap",
    "gridRowStart",
    "gridTemplate",
    "gridTemplateAreas",
    "gridTemplateColumns",
    "gridTemplateRows",
    "height",
    "hyphens",
    "inlineSize",
    "inset",
    "justifyContent",
    "justifyItems",
    "justifySelf",
    "left",
    "letterSpacing",
    "lineHeight",
    "listStyle",
    "listStyleImage",
    "listStylePosition",
    "listStyleType",
    "margin",
    "marginBottom",
    "marginLeft",
    "marginRight",
    "marginTop",
    "mask",
    "maxHeight",
    "maxWidth",
    "minHeight",
    "minWidth",
    "mixBlendMode",
    "objectFit",
    "objectPosition",
    "opacity",
    "order",
    "outline",
    "outlineColor",
    "outlineOffset",
    "outlineStyle",
    "outlineWidth",
    "overflow",
    "overflowWrap",
    "overflowX",
    "overflowY",
    "padding",
    "paddingBottom",
    "paddingLeft",
    "paddingRight",
    "paddingTop",
    "pageBreakAfter",
    "pageBreakBefore",
    "pageBreakInside",
    "perspective",
    "perspectiveOrigin",
    "placeContent",
    "placeItems",
    "placeSelf",
    "pointerEvents",
    "position",
    "quotes",
    "resize",
    "right",
    "rowGap",
    "scrollBehavior",
    "tabSize",
    "tableLayout",
    "textAlign",
    "textAlignLast",
    "textCombineUpright",
    "textDecoration",
    "textDecorationColor",
    "textDecorationLine",
    "textDecorationStyle",
    "textIndent",
    "textJustify",
    "textOrientation",
    "textOverflow",
    "textShadow",
    "textTransform",
    "top",
    "transform",
    "transformOrigin",
    "transformStyle",
    "transition",
    "transitionDelay",
    "transitionDuration",
    "transitionProperty",
    "transitionTimingFunction",
    "unicodeBidi",
    "userSelect",
    "verticalAlign",
    "visibility",
    "whiteSpace",
    "width",
    "willChange",
    "wordBreak",
    "wordSpacing",
    "wordWrap",
    "writingMode",
    "zIndex",
};

/* CSS.supports(): recognise a property by name */
static void css_norm(const char *s, char *out, int cap)
{
  int o = 0; /* lowercase, strip dashes */
  for (int i = 0; s[i] && o < cap - 1; i++) {
    char c = s[i];
    if (c == '-')
      continue;
    out[o++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  out[o] = 0;
}
static bool css_prop_known(const char *prop)
{
  /* The table is a compile-time constant, so normalize it once and reuse. */
  static char norm_props[sizeof k_css_props / sizeof k_css_props[0]][64];
  static bool norm_built;
  char pn[64];
  css_norm(prop, pn, sizeof pn);
  if (!pn[0])
    return false;
  if (!norm_built) {
    for (unsigned i = 0; i < sizeof k_css_props / sizeof k_css_props[0]; i++)
      css_norm(k_css_props[i], norm_props[i], sizeof norm_props[i]);
    norm_built = true;
  }
  for (unsigned i = 0; i < sizeof k_css_props / sizeof k_css_props[0]; i++)
    if (streq(pn, norm_props[i]))
      return true;
  return false;
}
js_value css_supports(js_ctx *ctx, js_value t, js_value *args, int argc)
{
  (void)t;
  if (argc >= 2) /* CSS.supports(prop, value) */
    return js_bool(css_prop_known(arg_str(ctx, A(0))));
  { /* CSS.supports("cond string") */
    const char *cond = arg_str(ctx, A(0));
    char prop[64];
    int i = 0;
    const char *p = cond;
    if (contains_ci(cond, "selector("))
      return js_bool(0);
    while (*p == ' ' || *p == '(')
      p++;
    while (*p && *p != ':' && i < 63)
      prop[i++] = *p++;
    while (i > 0 && prop[i - 1] == ' ')
      i--;
    prop[i] = 0;
    if (*p != ':' || i == 0)
      return js_bool(0);
    return js_bool(css_prop_known(prop));
  }
}

void setup_style_proto(void)
{
  /* CSSStyleDeclaration-lite: accessors + methods over the style="" attribute */
  /* Full standard CSS property set (generic style-attr backed accessors) + length */
  for (unsigned pi = 0; pi < sizeof k_css_props / sizeof k_css_props[0]; pi++)
    def_style_prop(k_css_props[pi]);
  def_acc(g_style_proto, "length", style_gen_length, 0);
  def_acc(g_style_proto, "color", style_get_color, style_set_color);
  def_acc(g_style_proto, "background", style_get_background, style_set_background);
  def_acc(g_style_proto, "backgroundColor", style_get_backgroundColor, style_set_backgroundColor);
  def_acc(g_style_proto, "backgroundImage", style_get_backgroundImage, style_set_backgroundImage);
  def_acc(g_style_proto, "backgroundPosition", style_get_backgroundPosition, style_set_backgroundPosition);
  def_acc(g_style_proto, "backgroundSize", style_get_backgroundSize, style_set_backgroundSize);
  def_acc(g_style_proto, "backgroundRepeat", style_get_backgroundRepeat, style_set_backgroundRepeat);
  def_acc(g_style_proto, "display", style_get_display, style_set_display);
  def_acc(g_style_proto, "width", style_get_width, style_set_width);
  def_acc(g_style_proto, "height", style_get_height, style_set_height);
  def_acc(g_style_proto, "fontWeight", style_get_fontWeight, style_set_fontWeight);
  def_acc(g_style_proto, "fontFamily", style_get_fontFamily, style_set_fontFamily);
  def_acc(g_style_proto, "textAlign", style_get_textAlign, style_set_textAlign);
  def_acc(g_style_proto, "textDecoration", style_get_textDecoration, style_set_textDecoration);
  def_acc(g_style_proto, "cssText", style_get_csstext, style_set_csstext);
  def_method(g_style_proto, "setProperty", style_setprop, 2);
  def_method(g_style_proto, "getPropertyValue", style_getprop, 1);
  def_method(g_style_proto, "removeProperty", style_removeprop, 1);
}
