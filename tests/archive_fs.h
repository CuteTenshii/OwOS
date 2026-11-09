/* Host stand-ins for the guest file calls user/lib/archive makes; scripts/tests.mk renames them onto these and
 * archive_test.c defines them. */
#ifndef OWOS_TEST_ARCHIVE_FS_H
#define OWOS_TEST_ARCHIVE_FS_H

int archive_test_mkdir(const char *path);
int archive_test_stream_begin(const char *path);
int archive_test_stream_write(const void *buf, int len);
int archive_test_stream_end(void);

#endif
