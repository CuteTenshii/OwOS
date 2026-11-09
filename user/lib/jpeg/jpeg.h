/* jpeg.h, baseline and progressive JPEG decoder (8-bit Huffman; grayscale and YCbCr/RGB, any subsampling) */
#ifndef OWOS_JPEG_H
#define OWOS_JPEG_H

enum {
  JPEG_OK = 0,
  JPEG_EFORMAT = -1, /* not a JPEG, or corrupt/truncated */
  JPEG_EUNSUP = -2,  /* arithmetic/lossless/12-bit/CMYK, or too large to decode */
  JPEG_ENOMEM = -3,
};

/* Decode `buf` into a malloc'd RGBA buffer (R,G,B,A bytes), returned via *out. When max_dim > 0 the image is
 * box-downscaled by 1, 2, 4 or 8 to the smallest size whose longer side is still >= max_dim; *w and *h
 * are the output size. On error *out is NULL. */
int jpeg_decode(const unsigned char *buf, int len, int max_dim, unsigned char **out, unsigned *w, unsigned *h);

#endif
