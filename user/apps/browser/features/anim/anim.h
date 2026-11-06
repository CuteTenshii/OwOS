/* anim.h: CSS animation and transition engine. State hangs off dom_node.anim_rec, worker thread only. */
#ifndef OWOS_ANIM_H
#define OWOS_ANIM_H

#include "css.h"
#include "dom.h"

/* Begin a layout walk at monotonic time `now` (ms); clears the "needs another frame" flag. */
void anim_frame_begin(unsigned now);

/* Overlay running keyframe animations/transitions for `el` onto its cascaded style *cs. */
void anim_apply(css_sheet *sheet, struct dom_node *el, struct css_style *cs, unsigned now);

/* 1 if any animation/transition still needs frames after the last walk. */
int anim_active(void);

/* Drop all animation state. */
void anim_reset(void);

#endif /* OWOS_ANIM_H */
