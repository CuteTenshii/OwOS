/* streamscore.h: the native half of the Streams API (streamscore.c). */
#ifndef BROWSER_JS_STREAMSCORE_H
#define BROWSER_JS_STREAMSCORE_H

#include "jsint.h"

/* First tag of the OBJ_HOST objects made here (canvasjs.h uses the small ones) */
#define HK_STREAM 100

/* Defines the stream classes on a new object and returns it: ReadableStream, ReadableStreamDefaultReader,
 * ReadableStreamDefaultController, WritableStream, WritableStreamDefaultWriter, WritableStreamDefaultController,
 * TransformStream, TransformStreamDefaultController, plus `after`, the job-ordered promise callback the
 * script half uses. Sets up this thread's prototypes, so the page and each worker call it for their own context. */
js_object *streams_core_install(js_ctx *ctx);

/* Releases the host struct of a stream object (the collector's finalizer calls this for tags from HK_STREAM) */
void streams_core_free(js_object *o);

#endif
