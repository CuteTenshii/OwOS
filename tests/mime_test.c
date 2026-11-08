/* Host test for the MIME helper (user/lib/mime/mime.c). */
#define OWOS_SUITE_TEST
#include "mime.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
	test("extension lookup");
	expect_string(mime_from_ext("/a/b/photo.JPG"), "image/jpeg");
	expect_string(mime_from_ext("dir.d/readme"), "application/octet-stream");
	expect_string(mime_from_ext("a.tar.gz"), "application/gzip");
	expect_string(mime_from_ext("font.woff2"), "font/woff2");
	expect_string(mime_from_ext("x.unknownext"), "application/octet-stream");

	test("reverse lookup");
	expect_string(mime_to_ext("image/jpeg"), "jpg");
	expect_string(mime_to_ext("Text/HTML; charset=utf-8"), "html");
	expect(mime_to_ext("application/x-nonexistent") == 0);
	expect(mime_to_ext("text/plai") == 0);

	test("table round trip");
	const char *ext, *mime;
	int n = 0, primary_ok = 1;
	for (int i = 0; mime_entry(i, &ext, &mime); i++, n++) {
		char name[64], back_name[64];
		snprintf(name, sizeof name, "f.%s", ext);
		const char *back = mime_to_ext(mime);
		if (!back) {
			primary_ok = 0;
			continue;
		}
		snprintf(back_name, sizeof back_name, "f.%s", back);
		if (strcmp(mime_from_ext(name), mime) != 0 || strcmp(mime_from_ext(back_name), mime) != 0)
			primary_ok = 0;
	}
	expect(primary_ok);
	expect(n > 150);
	expect(mime_entry(-1, &ext, &mime) == 0);
	expect(mime_entry(n, &ext, &mime) == 0);

	test("textual types");
	expect(mime_is_text("text/x-python"));
	expect(mime_is_text("application/atom+xml"));
	expect(mime_is_text("application/ld+json"));
	expect(!mime_is_text("image/png"));
	expect(!mime_is_text("application/zip"));
	return 0;
}
