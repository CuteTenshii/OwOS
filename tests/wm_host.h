/* Host stand-ins for the syscalls the window client (user/lib/win/win.c) and server (compositor/wmserver.c) make.
 * Force-included into both, and into wm_test.c after its system headers; the fakes live in wm_test.c. */
#ifndef OWOS_TEST_WM_HOST_H
#define OWOS_TEST_WM_HOST_H

#include "uabi.h"

#define read wmt_read
#define write wmt_write
#define close wmt_close
#define read_nb wmt_read_nb
#define write_nb wmt_write_nb
#define msleep wmt_msleep
#define port_open wmt_port_open
#define port_accept_nb wmt_port_accept_nb
#define port_peer wmt_port_peer
#define shm_create wmt_shm_create
#define shm_grant wmt_shm_grant
#define shm_map wmt_shm_map
#define shm_unmap wmt_shm_unmap
#define shm_release wmt_shm_release

/* Mirrors of user/lib/owos.h, which can't compile on the host */
#define WEV_KEY 1
#define WEV_MOUSE 2
#define WEV_CLOSE 3
#define WEV_RESIZE 4
#define WEV_DBLCLICK 5
#define WEV_RCLICK 6
#define WEV_SCROLL 7
#define WEV_DRAG 8
#define WEV_MOVE 9
#define WIN_CURSOR_PROGRESS 14

struct win_event {
  unsigned type;
  int a, b;
};

struct port_peer {
  int pid, uid, gid;
};

int read(int fd, void *buf, int len);
int write(int fd, const void *buf, int len);
int close(int fd);
int read_nb(int fd, void *buf, int len);
int write_nb(int fd, const void *buf, int len);
void msleep(unsigned ms);
int port_open(const char *name, int as_server);
int port_accept_nb(int listen_fd);
int port_peer(int fd, struct port_peer *out);
int shm_create(unsigned bytes);
int shm_grant(int handle, int pid);
int shm_map(int handle, sys_word addr);
int shm_unmap(sys_word addr);
int shm_release(int handle);

int win_create(int w, int h, const char *title);
int win_present(int id);
int win_present_rect(int id, int x, int y, int w, int h);
int win_poll(int id, struct win_event *ev);
int win_close(int id);
int win_owned(int pid);
int win_set_parent(int id, int parent, int modal);
unsigned *win_fb_of(int id);
int win_settitle(int id, const char *title);

#endif
