/* bmedia.h: the browser's <audio>/<video> HTMLMediaElement layer on top of user/lib/media. */
#ifndef OWOS_BMEDIA_H
#define OWOS_BMEDIA_H

struct dom_node;
struct media_el;

/* canPlayType / source selection verdicts. */
enum { MEDIA_CANT = 0, MEDIA_MAYBE = 1, MEDIA_PROBABLY = 2 };

/* readyState (HAVE_*) and networkState (NETWORK_*) constants. */
enum { MRS_NOTHING = 0, MRS_METADATA = 1, MRS_CURRENT = 2, MRS_FUTURE = 3, MRS_ENOUGH = 4 };
enum { MNS_EMPTY = 0, MNS_IDLE = 1, MNS_LOADING = 2, MNS_NO_SOURCE = 3 };

/* html.c: register a media element; `text_pos` anchors the controls widget (<0 = none). */
struct media_el *media_register_element(struct dom_node *node, int text_pos, const char *page_url);

/* js.c: HTMLMediaElement API (worker thread) */
struct media_el *media_for_node(struct dom_node *node);  /* find-or-create for audio/video */
struct media_el *media_new_audio(struct dom_node *node); /* new Audio(): a detached element */

void media_set_src(struct media_el *m, const char *abs_url); /* absolute URL */
void media_get_src(struct media_el *m, char *out, int cap);
void media_play(struct media_el *m);
void media_pause(struct media_el *m);
void media_load(struct media_el *m); /* (re)start loading */
int media_paused(struct media_el *m);
int media_ended(struct media_el *m);
double media_current_time(struct media_el *m);
void media_set_current_time(struct media_el *m, double sec);
double media_duration(struct media_el *m); /* NaN if unknown */
double media_volume(struct media_el *m);   /* 0.0 .. 1.0 */
void media_set_volume(struct media_el *m, double v);
int media_muted(struct media_el *m);
void media_set_muted(struct media_el *m, int on);
int media_loop_get(struct media_el *m);
void media_set_loop(struct media_el *m, int on);
int media_autoplay_get(struct media_el *m);
void media_set_autoplay(struct media_el *m, int on);
int media_ready_state(struct media_el *m);
int media_network_state(struct media_el *m);
int media_can_play_type(const char *type); /* MEDIA_* */

/* Fire queued media events; returns the number dispatched. */
int media_pump(void);

/* Implemented in js.c: fire a non-bubbling media event on a node. */
void js_media_fire(struct dom_node *node, const char *type);

/* main.c: layout, render and input for the controls widget */
void media_reset_layout(void);                    /* rewrap start: clear placements */
struct media_el *media_at_text_pos(int text_pos); /* doc media anchored at pos, or NULL */
int media_widget_rows(struct media_el *m, int paw, int line_h);
void media_set_layout(struct media_el *m, int start_row, int rows);
struct media_el *media_doc_first(void);
struct media_el *media_doc_next(struct media_el *m);
int media_el_start_row(struct media_el *m);
int media_el_rows(struct media_el *m);
int media_el_height(struct media_el *m, int paw); /* widget pixel height */
void media_el_draw(struct media_el *m, int x0, int y0, int paw, int ct, int cb);
void media_el_click(struct media_el *m, int mx, int my, int x0, int y0, int paw);
struct media_el *media_fullscreen(void); /* the <video> that is, or sits inside, the fullscreen element; else NULL */
void media_fs_draw(struct media_el *m, int x0, int y0, int w, int h, int ct, int cb);
void media_fs_click(struct media_el *m, int mx, int my, int x0, int y0, int w, int h);
int media_active(void);               /* any owned media is playing */
struct media_el *media_current(void); /* the page's playing element, else the last one played, or NULL */

/* free every media element owned by the current page */
void media_page_teardown(void);

#endif /* OWOS_BMEDIA_H */
