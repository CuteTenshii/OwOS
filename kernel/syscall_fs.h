#ifndef OWOS_SYSCALL_FS_H
#define OWOS_SYSCALL_FS_H

struct registers;

void syscall_fs_stream_begin(struct registers *r);
void syscall_fs_stream_write(struct registers *r);
void syscall_fs_stream_end(struct registers *r);
void syscall_fs_mkdir(struct registers *r);
void syscall_fs_rmdir(struct registers *r);
void syscall_fs_delete(struct registers *r);
void syscall_fs_rename(struct registers *r);
void syscall_fs_chdir(struct registers *r);
void syscall_fs_readfile(struct registers *r);
void syscall_fs_writefile(struct registers *r);
void syscall_fs_readfile_at(struct registers *r);

#endif
