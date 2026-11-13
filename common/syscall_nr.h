#ifndef OWOS_SYSCALL_NR_H
#define OWOS_SYSCALL_NR_H

#define SYS_WRITE 1
#define SYS_EXIT 2
#define SYS_READ 3
#define SYS_FORK 4
#define SYS_READFILE 5
#define SYS_EXEC 6
#define SYS_WAITPID 7
#define SYS_PIPE 8
#define SYS_CLOSE 9
#define SYS_WRITEFILE 10
#define SYS_NET_CONNECT 11
#define SYS_NET_SEND 12
#define SYS_NET_RECV 13
#define SYS_NET_CLOSE 14
#define SYS_WIN_CREATE 15
#define SYS_WIN_PRESENT 16
#define SYS_WIN_POLL 17
#define SYS_WIN_CLOSE 18
#define SYS_READDIR 19
#define SYS_WIN_TITLE 20
#define SYS_THREAD 21
#define SYS_SLEEP 22
#define SYS_WIN_RESIZABLE 23
#define SYS_MSEC 24
#define SYS_READFILE_AT 25
#define SYS_VOLUMES 26
#define SYS_UMOUNT 27
#define SYS_WIN_CURSOR 28
#define SYS_STREAM_BEGIN 29
#define SYS_STREAM_WRITE 30
#define SYS_STREAM_END 31
#define SYS_MKDIR 32
#define SYS_RMDIR 33
#define SYS_DELETE 34
#define SYS_RENAME 35
#define SYS_STAT 36
#define SYS_SPAWN 37
#define SYS_MOUNT 38
#define SYS_AUDIO_OPEN 39
#define SYS_AUDIO_WRITE 40
#define SYS_AUDIO_CLOSE 41
#define SYS_SPAWN_CAP 42
#define SYS_READ_NB 43
#define SYS_CHDIR 44
#define SYS_KILL 45
#define SYS_POWER 46
#define SYS_SBRK 47
#define SYS_GETUID 48
#define SYS_PORT_OPEN 49
#define SYS_PORT_ACCEPT 50
#define SYS_RTC_EPOCH 51
#define SYS_NET_SETOPT 52
#define SYS_NET_LISTEN 53
#define SYS_NET_ACCEPT 54
#define SYS_NET_BIND 55
#define SYS_STATFS 56
#define SYS_IFCONFIG 57
#define SYS_SETRTC 58
#define SYS_FB_INFO 59
#define SYS_FB_MAP 60
#define SYS_FB_PRESENT 61
#define SYS_INPUT_POLL 62
#define SYS_INPUT_SET_BOUNDS 63
#define SYS_WIN_ENUM 64
#define SYS_WIN_MAP 65
#define SYS_WIN_UNMAP 66
#define SYS_WIN_SET_GEOM 67
#define SYS_WIN_DELIVER 68
#define SYS_CRASH_POLL 69
#define SYS_WIN_KEY_SPACE 70
#define SYS_STACK_ALLOC 71
#define SYS_STACK_FREE 72
#define SYS_SIGACTION 73
#define SYS_SIGPROCMASK 74
#define SYS_SIGRETURN 75
#define SYS_GETPID 76
#define SYS_GETRANDOM 77
#define SYS_WIN_OWNS 78
#define SYS_WIN_SET_PARENT 79
#define SYS_WIN_FB 80
#define SYS_UNAME 81
#define SYS_FUTEX_WAIT 82
#define SYS_FUTEX_WAKE 83
#define SYS_GETTIME 84
#define SYS_SETTIME 85
#define SYS_ADJTIME 86
#define SYS_CHMOD 87
#define SYS_CHOWN 88
#define SYS_UMASK 89
#define SYS_ACCESS 90
#define SYS_TTY 91
#define SYS_ENV 92
#define SYS_FB_CURSOR 93
#define SYS_WIN_FULLSCREEN 95 /* ebx = window id, ecx = on */
#define SYS_SET_TLS 94        /* ebx = thread pointer: the base of the %gs segment (0 = none) */
#define SYS_SYMBOLIZE 96      /* ebx = address in the caller's program, ecx = out, edx = cap: "func+0xNN", 0/-1 */
#define SYS_THREAD_NAME 97    /* ebx = THREAD_NAME_SET (ecx = name, NULL clears) or _GET (ecx = out, edx = cap) */
#define THREAD_NAME_SET 0
#define THREAD_NAME_GET 1
#define THREAD_NAME_MAX 32 /* label bytes including the NUL; longer names are truncated */

/* SYS_ENV commands (ebx points at a struct env_req: cmd, name, name_len, value, value_len, buf, buf_len, index) */
#define ENV_GET 0   /* copy the value of name into buf (if buf_len > 0); returns its length, -1 if unset */
#define ENV_SET 1   /* set name to value; index != 0 overwrites an existing one */
#define ENV_UNSET 2 /* remove name */
#define ENV_ENTRY 3 /* copy the index-th "NAME=VALUE" into buf; returns its length, -1 past the end */
#define ENV_CLEAR 4 /* remove everything */

struct env_req {
  unsigned cmd;
  unsigned name, name_len;
  unsigned value, value_len;
  unsigned buf, buf_len;
  unsigned index;
};

/* SYS_SPAWN_CAP request flags */
#define SPAWN_PTY 1   /* give the child a pty as fds 0-2 and return the master fd in *rfd */
#define SPAWN_STDIO 2 /* child fds 0-2 are copies of the caller's fds named in stdio[] (-1: left as the default) */
#define SPAWN_CREDS 8 /* user library only: apply spawn_opts.uid/gid */
#define SPAWN_ENV 4   /* the child gets the NAME=VALUE block at env (env_len bytes) instead of a copy of ours */

/* SYS_TTY commands (fd in ebx, command in ecx, argument in edx) */
#define TTY_ISATTY 0  /* 1 if fd is a terminal */
#define TTY_GETSIZE 1 /* cols << 16 | rows */
#define TTY_SETSIZE 2 /* pty master only; arg = cols << 16 | rows */
#define TTY_SETFG 3   /* record the foreground job pid (0 = the shell itself) */
#define TTY_GETFG 4   /* the recorded foreground pid */

#endif
