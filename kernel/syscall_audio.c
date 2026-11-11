#include "syscall_audio.h"
#include "audio.h"
#include "idt.h"
#include "sched.h"
#include "uaccess.h"
#include <stdint.h>

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
  __asm__ volatile("sti");
  r->eax = (uint32_t)audio_backend->stream_write((const void *)(uintptr_t)buf, len);
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
