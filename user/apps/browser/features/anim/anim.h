/* anim.h — CSS animation & transition engine for the browser.
 *
 * The renderer produces one `struct css_style` per element per layout walk and
 * draws everything from it, so animation reduces to: interpolate that style
 * over time and re-run the walk each frame. anim_apply() overlays a running
 * @keyframes animation and/or CSS transition onto a freshly-cascaded style;
 * anim_active() tells the event loop whether another frame is due, so it keeps
 * relaying out while anything is in motion.
 *
 * All state hangs off the dom_node (dom_node.anim_rec) and lives on the
 * worker thread — the only thread that walks the DOM and touches the JS/CSS
 * engine. No locking beyond the existing layout discipline. */
#ifndef OWOS_ANIM_H
#define OWOS_ANIM_H

#include "css.h"
#include "dom.h"

/* Begin a layout walk at monotonic time `now` (ms): clears the "needs another
 * frame" flag that anim_apply() re-raises. */
void anim_frame_begin(unsigned now);

/* Overlay any running keyframe animation / transition for `el` onto its
 * computed style *cs (already cascaded). `sheet` supplies @keyframes. Records
 * the element's base style so a later change to a transitioned property starts
 * a transition. Call once per element per walk, right after compute_style();
 * elements that never animate cost one pointer test. */
void anim_apply(css_sheet *sheet, struct dom_node *el, struct css_style *cs, unsigned now);

/* 1 if any animation/transition still needs frames — reflects the walk just
 * finished. The event loop uses it to schedule the next relayout. */
int anim_active(void);

/* Drop all animation state (page teardown / navigation). */
void anim_reset(void);

#endif /* OWOS_ANIM_H */
