#include "syscall_audio.h"
#include "audio.h"
#include "heap.h"
#include "idt.h"
#include "sched.h"
#include "uaccess.h"
#include <stdint.h>

#define AUDIO_CHUNK 4096u

static const struct audio_card *audio_backend;
static uint32_t audio_owner;

void audio_task_exiting(uint32_t id)
{
  if (!audio_backend || audio_owner != id)
    return;
  audio_backend->stream_close();
  audio_backend = 0;
  audio_owner = 0;
}

void syscall_audio_open(struct registers *r)
{
  uint32_t owner = sched_current_id();
  if (audio_backend && audio_owner != owner) {
    r->eax = (uint32_t)-1;
    return;
  }
  const struct audio_card *card = audio_stream_card();
  if (card) {
    __asm__ volatile("sti");
    audio_backend = card->stream_open() == 0 ? card : 0;
  } else
    audio_backend = 0;
  audio_owner = audio_backend ? owner : 0;
  r->eax = audio_backend ? 0 : (uint32_t)-1;
}

void syscall_audio_write(struct registers *r)
{
  uint32_t buf = r->ebx, len = r->ecx;
  if (len == 0 || !user_range(buf, len) || !audio_backend || audio_owner != sched_current_id()) {
    r->eax = (uint32_t)-1;
    return;
  }
  /* stream_write waits for DMA room with interrupts on, so a sibling thread can unmap `buf`: stage it in chunks. */
  uint8_t *kbuf = kmalloc(AUDIO_CHUNK);
  if (!kbuf) {
    r->eax = (uint32_t)-1;
    return;
  }
  __asm__ volatile("sti");
  uint32_t done = 0;
  int err = 0;
  while (done < len && audio_backend && audio_owner == sched_current_id()) {
    uint32_t n = len - done < AUDIO_CHUNK ? len - done : AUDIO_CHUNK;
    if (copyin(kbuf, buf + done, n) != 0) {
      err = 1;
      break;
    }
    int w = audio_backend->stream_write(kbuf, n);
    if (w < 0) {
      err = 1;
      break;
    }
    done += (uint32_t)w;
    if ((uint32_t)w < n)
      break;
  }
  kfree(kbuf);
  r->eax = done || !err ? done : (uint32_t)-1;
}

void syscall_audio_close(struct registers *r)
{
  if (!audio_backend || audio_owner != sched_current_id()) {
    r->eax = 0;
    return;
  }
  __asm__ volatile("sti");
  audio_backend->stream_close();
  audio_backend = 0;
  audio_owner = 0;
  r->eax = 0;
}
