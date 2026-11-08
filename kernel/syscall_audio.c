#include "syscall_audio.h"
#include "bt.h"
#include "hda.h"
#include "idt.h"
#include "sb16.h"
#include "sched.h"
#include "uaccess.h"
#include <stdint.h>

static int audio_backend;
static uint32_t audio_owner;

void audio_task_exiting(uint32_t id)
{
  if (audio_backend == 0 || audio_owner != id)
    return;
  if (audio_backend == 1)
    hda_stream_close();
  else if (audio_backend == 2)
    sb16_stream_close();
  else if (audio_backend == 3)
    bt_a2dp_close();
  audio_backend = 0;
  audio_owner = 0;
}

void syscall_audio_open(struct registers *r)
{
  uint32_t owner = sched_current_id();
  if (audio_backend != 0 && audio_owner != owner) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (bt_a2dp_ready()) {
    __asm__ volatile("sti");
    audio_backend = bt_a2dp_open() == 0 ? 3 : 0;
  } else if (hda_present())
    audio_backend = hda_stream_open() == 0 ? 1 : 0;
  else if (sb16_present())
    audio_backend = sb16_stream_open() == 0 ? 2 : 0;
  else
    audio_backend = 0;
  audio_owner = audio_backend ? owner : 0;
  r->eax = audio_backend ? 0 : (uint32_t)-1;
}

void syscall_audio_write(struct registers *r)
{
  uint32_t buf = r->ebx, len = r->ecx;
  if (len == 0 || !user_range(buf, len) || audio_backend == 0 || audio_owner != sched_current_id()) {
    r->eax = (uint32_t)-1;
    return;
  }
  __asm__ volatile("sti");
  if (audio_backend == 1)
    r->eax = (uint32_t)hda_stream_write((const void *)(uintptr_t)buf, len);
  else if (audio_backend == 2)
    r->eax = (uint32_t)sb16_stream_write((const void *)(uintptr_t)buf, len);
  else if (audio_backend == 3)
    r->eax = (uint32_t)bt_a2dp_write((const void *)(uintptr_t)buf, len);
  else
    r->eax = (uint32_t)-1;
}

void syscall_audio_close(struct registers *r)
{
  if (audio_backend == 0 || audio_owner != sched_current_id()) {
    r->eax = 0;
    return;
  }
  __asm__ volatile("sti");
  if (audio_backend == 1)
    hda_stream_close();
  else if (audio_backend == 2)
    sb16_stream_close();
  else if (audio_backend == 3)
    bt_a2dp_close();
  audio_backend = 0;
  audio_owner = 0;
  r->eax = 0;
}
