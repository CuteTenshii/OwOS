#ifndef BROWSER_MEDIASTREAM_H
#define BROWSER_MEDIASTREAM_H

#include "jsint.h"

/* A numeric constraint: a bare number is `ideal`; the others come from the {ideal, exact, min, max} dictionary. */
struct ms_range {
  bool ideal_set, exact_set, min_set, max_set;
  double ideal, exact, min, max;
};

/* A device's choice among its modes, kept on each track so getSettings still works after the track ended. */
struct ms_pick {
  int v[4];
};

/* What a capture device (camera, microphone) offers the shared MediaStream layer. */
struct ms_device {
  const char *kind;  /* track kind and constraint key: "video" or "audio" */
  const char *input; /* enumerateDevices kind */
  int feature;       /* the permission it needs */
  void (*init)(js_ctx *ctx, const char *page_url);
  bool (*present)(void); /* the device exists; refreshes what it reports */
  const char *(*label)(void);
  const char *(*id)(void);
  /* NULL and *out set, or the name of a constraint nothing satisfies. `shared`: other tracks are using the device. */
  const char *(*choose)(js_value constraints, bool shared, struct ms_pick *out);
  bool (*open)(const struct ms_pick *pick); /* start the device, or switch the running one to `pick` */
  void (*close)(void);
  bool (*running)(void);
  void (*settings)(js_object *out, const struct ms_pick *pick);
  void (*capabilities)(js_object *out);
  bool (*tick)(unsigned now); /* runs while a track is live; false once the device has gone away */
  void (*teardown)(void);
};

/* Define navigator.mediaDevices, MediaStream and MediaStreamTrack (secure contexts only). */
void mediastream_install(js_ctx *ctx, js_object *navigator, const char *page_url);
/* Worker loop: run the live devices; returns nonzero if page script ran. */
int mediastream_tick(void);
/* Page unload: stop every device, end every track. */
void mediastream_teardown(void);
/* True if `v` is a MediaStream (what HTMLMediaElement.srcObject accepts). */
bool mediastream_is_stream(js_value v);

/* For the devices: live tracks of a kind, whether any is enabled, and a track's state by serial. */
int mediastream_live(const char *kind);
bool mediastream_any_enabled(const char *kind);
unsigned mediastream_track_serial(js_value track); /* 0 unless `track` is a live track */
bool mediastream_serial_live(unsigned serial);
bool mediastream_serial_enabled(unsigned serial);
void mediastream_read_range(js_value v, struct ms_range *r);
bool mediastream_in_range(const struct ms_range *r, double v); /* exact, min and max (ideal is only a preference) */
/* False if the constraints demand (deviceId.exact, a string or an array) some other device than `id` */
bool mediastream_device_matches(js_value constraints, const char *id);
js_value mediastream_range(double lo, double hi); /* {min, max}, for capabilities */
/* Origin-scoped device id (16 hex digits) so two sites cannot correlate a device through it */
void mediastream_device_id(const char *page_url, const char *salt, char out[17]);

#endif
