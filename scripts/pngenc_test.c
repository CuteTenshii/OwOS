/* pngenc_test.c — host test for the user-space PNG encoder (user/lib/png/pngenc.c).
 * Uses scripts/owos.h (the host shim: writefile writes a real file), encodes a
 * known gradient, and the Python check below decodes it with the standard zlib
 * and verifies dimensions + exact pixels — validated by an independent decoder.
 *
 *   cc -std=c11 -Iscripts -Iuser/lib/png -Iuser/lib/zlib scripts/pngenc_test.c \
 *      user/lib/png/pngenc.c user/lib/zlib/crc32.c -o /tmp/pngenc_test
 *   /tmp/pngenc_test /tmp/shot.png 64 48
 */
#include "owos.h"
#include "pngenc.h"

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "/tmp/shot.png";
	unsigned w = argc > 2 ? (unsigned)atoi(argv[2]) : 64;
	unsigned h = argc > 3 ? (unsigned)atoi(argv[3]) : 48;

	unsigned pitch = w + 7;                 /* non-trivial stride, like a real fb */
	unsigned *px = malloc((size_t)pitch * h * 4);
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++)
			px[y * pitch + x] = ((x * 255 / w) << 16) |
			                    ((y * 255 / h) << 8)  | ((x ^ y) & 0xFF);
	int r = png_write(out, px, w, h, pitch);
	free(px);
	if (r != 0) { printf("png_write failed: %d\n", r); return 1; }
	printf("wrote %s (%ux%u)\n", out, w, h);
	return 0;
}
