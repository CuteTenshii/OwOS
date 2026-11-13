#ifndef OWOS_SYSCALL_FS_H
#define OWOS_SYSCALL_FS_H

#include <stdint.h>

struct registers;

void syscall_fs_stream_begin(struct registers *r);
void syscall_fs_stream_write(struct registers *r);
void syscall_fs_stream_end(struct registers *r);
void syscall_fs_process_exiting(uint32_t pid); /* aborts a stream the process left open */
void syscall_fs_mkdir(struct registers *r);
void syscall_fs_rmdir(struct registers *r);
void syscall_fs_delete(struct registers *r);
void syscall_fs_rename(struct registers *r);
void syscall_fs_chdir(struct registers *r);
void syscall_fs_readfile(struct registers *r);
void syscall_fs_writefile(struct registers *r);
void syscall_fs_readfile_at(struct registers *r);
void syscall_fs_rtc_epoch(struct registers *r);
void syscall_fs_readdir(struct registers *r);
void syscall_fs_volumes(struct registers *r);
void syscall_fs_mount(struct registers *r);
void syscall_fs_install(struct registers *r);
void syscall_fs_umount(struct registers *r);
void syscall_fs_stat(struct registers *r);
void syscall_fs_statfs(struct registers *r);
void syscall_fs_uname(struct registers *r);
void syscall_fs_chmod(struct registers *r);
void syscall_fs_chown(struct registers *r);
void syscall_fs_umask(struct registers *r);
void syscall_fs_access(struct registers *r);

#endif
