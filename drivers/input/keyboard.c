/* keyboard.c - translate PS/2 scancodes (set 1, US QWERTY) and echo them. */

#include "keyboard.h"
#include "audio.h"
#include "fb.h"
#include "io.h"
#include "irq.h"
#include "keymap.h"
#include "serial.h"
#include "spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64
#define KBD_BUF_SIZE 128 /* power of two not required; SPSC ring    */

/* Scancodes we care about beyond plain keys. A key release is the make code
 * with bit 7 set. */
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL 0x1D /* left Ctrl; right Ctrl is 0xE0 0x1D       */
#define SC_CAPSLOCK 0x3A
#define SC_NUMLOCK 0x45
#define SC_EXTENDED 0xE0
#define SC_RELEASE 0x80

/* Extended (0xE0-prefixed) make codes for the arrow/navigation cluster. */
#define SC_E_UP 0x48
#define SC_E_DOWN 0x50
#define SC_E_LEFT 0x4B
#define SC_E_RIGHT 0x4D
#define SC_E_HOME 0x47
#define SC_E_END 0x4F
#define SC_E_DELETE 0x53
#define SC_ALT 0x38 /* extended 0xE0 0x38 is AltGr (right Alt)  */

/* This laptop's keyboard delivers its volume/brightness keys as bare function-
 * key make codes (set 1), not the E0 consumer codes above -- the EC doesn't
 * translate them. So F5/F6/F8/F9 double as brightness/volume and act directly
 * in the IRQ, same handlers as the E0 cases. (No Fn distinction reaches us, so
 * these keys are media keys system-wide; F5 therefore no longer sends KEY_F5.)
 */
#define SC_F5 0x3F  /* brightness down                          */
#define SC_F6 0x40  /* brightness up                            */
#define SC_F8 0x42  /* volume down                              */
#define SC_F9 0x43  /* volume up                                */
#define SC_F12 0x58 /* delivered to apps as KEY_F12 (devtools)  */

/* Numeric keypad make codes are contiguous from 0x47 to 0x53. */
#define SC_KP_FIRST 0x47
#define SC_KP_LAST 0x53

/* Scancode -> ASCII, unshifted. Unmapped entries are 0 (ignored). */
static const char keymap[128] = {
    [0x01] = 27,   [0x02] = '1', [0x03] = '2', [0x04] = '3',  [0x05] = '4', [0x06] = '5',  [0x07] = '6',  [0x08] = '7',
    [0x09] = '8',  [0x0A] = '9', [0x0B] = '0', [0x0C] = '-',  [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t', [0x10] = 'q',
    [0x11] = 'w',  [0x12] = 'e', [0x13] = 'r', [0x14] = 't',  [0x15] = 'y', [0x16] = 'u',  [0x17] = 'i',  [0x18] = 'o',
    [0x19] = 'p',  [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n', [0x1E] = 'a', [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',
    [0x22] = 'g',  [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',  [0x26] = 'l', [0x27] = ';',  [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c',  [0x2F] = 'v', [0x30] = 'b',  [0x31] = 'n',  [0x32] = 'm',
    [0x33] = ',',  [0x34] = '.', [0x35] = '/', [0x37] = '*',  [0x39] = ' ',
};

/* Scancode -> ASCII, with Shift held. */
static const char keymap_shift[128] = {
    [0x01] = 27,  [0x02] = '!', [0x03] = '@', [0x04] = '#',  [0x05] = '$', [0x06] = '%',  [0x07] = '^',  [0x08] = '&',
    [0x09] = '*', [0x0A] = '(', [0x0B] = ')', [0x0C] = '_',  [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t', [0x10] = 'Q',
    [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',  [0x15] = 'Y', [0x16] = 'U',  [0x17] = 'I',  [0x18] = 'O',
    [0x19] = 'P', [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n', [0x1E] = 'A', [0x1F] = 'S',  [0x20] = 'D',  [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',  [0x26] = 'L', [0x27] = ':',  [0x28] = '"',  [0x29] = '~',
    [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C',  [0x2F] = 'V', [0x30] = 'B',  [0x31] = 'N',  [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x37] = '*',  [0x39] = ' ',
};

/* French AZERTY. Same physical scancodes, reinterpreted: the letter block is
 * permuted (a<->q, z<->w, m moved) and the number row types symbols unshifted,
 * digits when shifted. Accented keys use their CP437 byte (é=0x82, è=0x8A,
 * ç=0x87, à=0x85, ù=0x97, ²=0xFD), which the VGA text font renders directly. */
static const char keymap_fr[128] = {
    [0x01] = 27,     [0x02] = '&', [0x03] = '\x82', [0x04] = '"',    [0x05] = '\'',   [0x06] = '(', [0x07] = '-',
    [0x08] = '\x8A', [0x09] = '_', [0x0A] = '\x87', [0x0B] = '\x85', [0x0C] = ')',    [0x0D] = '=', [0x0E] = '\b',
    [0x0F] = '\t',   [0x10] = 'a', [0x11] = 'z',    [0x12] = 'e',    [0x13] = 'r',    [0x14] = 't', [0x15] = 'y',
    [0x16] = 'u',    [0x17] = 'i', [0x18] = 'o',    [0x19] = 'p',    [0x1A] = '^',    [0x1B] = '$', [0x1C] = '\n',
    [0x1E] = 'q',    [0x1F] = 's', [0x20] = 'd',    [0x21] = 'f',    [0x22] = 'g',    [0x23] = 'h', [0x24] = 'j',
    [0x25] = 'k',    [0x26] = 'l', [0x27] = 'm',    [0x28] = '\x97', [0x29] = '\xFD', [0x2B] = '*', [0x2C] = 'w',
    [0x2D] = 'x',    [0x2E] = 'c', [0x2F] = 'v',    [0x30] = 'b',    [0x31] = 'n',    [0x32] = ',', [0x33] = ';',
    [0x34] = ':',    [0x35] = '!', [0x37] = '*',    [0x39] = ' ',    [0x56] = '<', /* ISO 102nd key, left of W on AZERTY
                                                                                    */
};

/* French AZERTY with Shift held. */
static const char keymap_fr_shift[128] = {
    [0x01] = 27,   [0x02] = '1',    [0x03] = '2', [0x04] = '3', [0x05] = '4',    [0x06] = '5',    [0x07] = '6',
    [0x08] = '7',  [0x09] = '8',    [0x0A] = '9', [0x0B] = '0', [0x0C] = '\xF8', [0x0D] = '+',    [0x0E] = '\b',
    [0x0F] = '\t', [0x10] = 'A',    [0x11] = 'Z', [0x12] = 'E', [0x13] = 'R',    [0x14] = 'T',    [0x15] = 'Y',
    [0x16] = 'U',  [0x17] = 'I',    [0x18] = 'O', [0x19] = 'P', [0x1A] = '"',    [0x1B] = '\x9C', [0x1C] = '\n',
    [0x1E] = 'Q',  [0x1F] = 'S',    [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',    [0x23] = 'H',    [0x24] = 'J',
    [0x25] = 'K',  [0x26] = 'L',    [0x27] = 'M', [0x28] = '%', [0x29] = '~',    [0x2B] = '\xE6', [0x2C] = 'W',
    [0x2D] = 'X',  [0x2E] = 'C',    [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N',    [0x32] = '?',    [0x33] = '.',
    [0x34] = '/',  [0x35] = '\x15', [0x37] = '*', [0x39] = ' ', [0x56] = '>', /* ISO 102nd key with Shift */
};

/* French AZERTY third level (AltGr / right Alt): the number row carries the
 * programmer's punctuation. Unmapped entries are 0, so AltGr falls back to the
 * plain key. */
static const char keymap_fr_altgr[128] = {
    [0x03] = '~',  [0x04] = '#', [0x05] = '{', [0x06] = '[', [0x07] = '|', [0x08] = '`',
    [0x09] = '\\', [0x0A] = '^', [0x0B] = '@', [0x0C] = ']', [0x0D] = '}',
};

/* Selectable layouts, keyed by the name `loadkeys` passes. altgr may be NULL
 * for layouts without a third level (e.g. plain US). */
struct kbd_layout {
  const char *name;
  const char *map;
  const char *shift;
  const char *altgr;
};

static const struct kbd_layout layouts[] = {
    {"us", keymap, keymap_shift, NULL},
    {"fr", keymap_fr, keymap_fr_shift, keymap_fr_altgr},
};

/* The keymaps in force; the handler indexes through these so a layout switch
 * is just a few pointer stores. Defaults to US. */
static const char *active_map = keymap;
static const char *active_shift = keymap_shift;
static const char *active_altgr = NULL;
static const char *active_layout = "us"; /* name, for /sys/class/input/kbd0 */

/* Keypad keys 0x47..0x53 in order, as they type when Num Lock is on. */
static const char keypad[] = {
    '7', '8', '9', '-', /* 0x47 0x48 0x49 0x4A */
    '4', '5', '6', '+', /* 0x4B 0x4C 0x4D 0x4E */
    '1', '2', '3',      /* 0x4F 0x50 0x51      */
    '0', '.',           /* 0x52 0x53           */
};

static bool shift_held;
static bool altgr_held;
static bool ctrl_held;
static bool alt_held;      /* left Alt, for Alt+Tab window switching   */
static bool alt_tabbed;    /* Tab was pressed during this Alt hold     */
static bool super_held;    /* GUI/Win key held; folds into KMOD_SUPER on any chord */
static bool super_chorded; /* a Super+key fired during this Super hold */
static bool expecting_extended;
static bool mute_held;       /* E0 20 make seen without its break, so typematic repeats don't re-toggle mute */
static bool num_lock = true; /* on by default, like most BIOSes        */
static bool caps_lock;       /* off by default                         */
static volatile uint32_t led_seq;
static spinlock_t kbd_lock; /* serialises the IRQ1 decoder with USB injection */

/* Set the keyboard LEDs (bit2 = Caps Lock, bit1 = Num Lock). The two ACK bytes
 * (0xFA) the keyboard sends back have bit 7 set, so the IRQ handler harmlessly
 * ignores them as "releases". */
static void keyboard_set_leds(void)
{
  led_seq++;
  uint8_t leds = (num_lock ? 0x02 : 0x00) | (caps_lock ? 0x04 : 0x00);
  for (int i = 0; i < 100000 && (inb(KBD_STATUS_PORT) & 0x02); i++)
    ;                        /* wait for the input buffer to drain      */
  outb(KBD_DATA_PORT, 0xED); /* "set LEDs" command                      */
  for (int i = 0; i < 100000 && (inb(KBD_STATUS_PORT) & 0x02); i++)
    ;
  outb(KBD_DATA_PORT, leds);
}

/* Single-producer (IRQ) / single-consumer (main loop) ring buffer. head is
 * only written by the handler, tail only by keyboard_poll(), so byte-sized
 * indices need no locking on a single core. */
static volatile char ring[KBD_BUF_SIZE];
static volatile uint8_t ringmod[KBD_BUF_SIZE]; /* KMOD_* held when ring[i] was pressed */
static volatile uint8_t ring_head;
static volatile uint8_t ring_tail;

/* Ctrl+C is a latch, not a queued key: it must interrupt a running command,
 * which is busy in a loop and never draining the ring. Consumers poll it. */
static volatile bool int_pending;

/* Print Screen is a latch too: the IRQ can't capture the framebuffer, so it
 * just flags the request; SYS_INPUT_POLL drains it into a KEY_PRTSC event and
 * the compositor (which owns the framebuffer) takes the shot. */
static volatile bool scrshot_pending;

/* The modifier set currently held, as KMOD_* flags. Sampled when a key is
 * queued so the consumer learns which chord produced it. */
static uint8_t cur_mods(void)
{
  uint8_t m = 0;
  if (shift_held)
    m |= KMOD_SHIFT;
  if (ctrl_held)
    m |= KMOD_CTRL;
  if (alt_held)
    m |= KMOD_ALT;
  if (super_held)
    m |= KMOD_SUPER;
  if (altgr_held)
    m |= KMOD_ALTGR;
  return m;
}

static void ring_push(char c)
{
  uint8_t next = (ring_head + 1) % KBD_BUF_SIZE;
  if (next != ring_tail) { /* drop the key if the buffer is full      */
    ring[ring_head] = c;
    ringmod[ring_head] = cur_mods();
    ring_head = next;
  }
}

int keyboard_poll(char *out, unsigned char *mods)
{
  if (ring_head == ring_tail)
    return 0;
  *out = ring[ring_tail];
  if (mods)
    *mods = ringmod[ring_tail];
  ring_tail = (ring_tail + 1) % KBD_BUF_SIZE;
  return 1;
}

int keyboard_poll_text(char *out, unsigned char *mods)
{
  while (keyboard_poll(out, mods))
    if ((unsigned char)*out < KEY_MEDIA_PLAY)
      return 1;
  return 0;
}

/* Returns 1 (and clears the latch) if Ctrl+C has been pressed since the last
 * call. The idle shell loop polls this to cancel the line being typed; a
 * long-running command polls it to abort. */
int keyboard_take_interrupt(void)
{
  bool v = int_pending;
  int_pending = false;
  return v;
}

/* Returns 1 (and clears the latch) if Print Screen has been pressed since the
 * last call. SYS_INPUT_POLL polls it and surfaces KEY_PRTSC to the compositor.
 */
int keyboard_take_screenshot(void)
{
  bool v = scrshot_pending;
  scrshot_pending = false;
  return v;
}

/* Raise the interrupt latch from outside the IRQ handler -- the serial console
 * uses this so a Ctrl+C typed in the host terminal stops a running command too.
 */
void keyboard_signal_interrupt(void) { int_pending = true; }

/* Volume and brightness act here so they work without a compositor; every special key is also queued for the OSD. */
static void special_key(uint8_t key)
{
  switch (key) {
  case KEY_MUTE:
    audio_volume_mute();
    break;
  case KEY_VOL_DOWN:
    audio_volume_step(-1);
    break;
  case KEY_VOL_UP:
    audio_volume_step(+1);
    break;
  case KEY_BRIGHT_DOWN:
    fb_brightness_step(-1);
    break;
  case KEY_BRIGHT_UP:
    fb_brightness_step(+1);
    break;
  }
  ring_push((char)key);
}

void keyboard_special_key(uint8_t key)
{
  uintptr_t f = spin_lock_irqsave(&kbd_lock);
  special_key(key);
  spin_unlock_irqrestore(&kbd_lock, f);
}

/* The byte after a 0xE0 prefix; unmapped extended keys are swallowed. */
static void process_extended(uint8_t sc)
{
  if (sc == SC_ALT) { /* AltGr down                             */
    altgr_held = true;
    return;
  }
  if (sc == (SC_ALT | SC_RELEASE)) { /* AltGr up                     */
    altgr_held = false;
    return;
  }
  if (sc == SC_CTRL) { /* right Ctrl down                        */
    ctrl_held = true;
    return;
  }
  if (sc == (SC_CTRL | SC_RELEASE)) { /* right Ctrl up                */
    ctrl_held = false;
    return;
  }
  if (sc == 0x37) {         /* Print Screen make (E0 2A E0 37)        */
    scrshot_pending = true; /* compositor drains it via SYS_INPUT_POLL */
    return;
  }
  if (sc == 0x5B || sc == 0x5C) { /* GUI/Win key down            */
    super_held = true;
    super_chorded = false;
    return; /* the Start-menu toggle waits for release  */
  }
  if (sc == (0x5B | SC_RELEASE) || sc == (0x5C | SC_RELEASE)) { /* Win up */
    if (!super_chorded)                                         /* a bare tap -> toggle Start menu */
      ring_push(KEY_SUPER);
    super_held = false;
    super_chorded = false;
    return;
  }
  if (sc == (0x20 | SC_RELEASE)) { /* mute up */
    mute_held = false;
    return;
  }
  if (sc & SC_RELEASE) /* other extended release: ignore         */
    return;
  uint8_t special = kbd_e0_key(sc);
  if (special) {
    if (special == KEY_MUTE) {
      if (mute_held)
        return;
      mute_held = true;
    }
    special_key(special);
    return;
  }
  switch (sc) {
  case 0x1C:
    ring_push('\n');
    break; /* keypad Enter  */
  case 0x35:
    ring_push('/');
    break; /* keypad /      */
  /* The navigation cluster emits the plain editing code; Shift and Alt ride
   * along in the modifier set (cur_mods), so a consumer extends a selection
   * on Shift+arrow or navigates on Alt+arrow itself. A bare Alt hold + arrow
   * doesn't set alt_tabbed, so releasing Alt afterwards emits no KEY_ALT_UP. */
  case SC_E_UP:
    ring_push(KEY_UP);
    break;
  case SC_E_DOWN:
    ring_push(KEY_DOWN);
    break;
  case SC_E_LEFT:
    ring_push(KEY_LEFT);
    break;
  case SC_E_RIGHT:
    ring_push(KEY_RIGHT);
    break;
  case SC_E_HOME:
    ring_push(KEY_HOME);
    break;
  case SC_E_END:
    ring_push(KEY_END);
    break;
  case SC_E_DELETE:
    ring_push(KEY_DELETE);
    break;
  }
}

/* Shift/Ctrl/Alt holds, Num/Caps lock, and the Alt+Tab gesture. 1 if consumed.
 */
static bool process_modifiers(uint8_t sc)
{
  if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
    shift_held = true;
    return true;
  }
  if (sc == (SC_LSHIFT | SC_RELEASE) || sc == (SC_RSHIFT | SC_RELEASE)) {
    shift_held = false;
    return true;
  }
  if (sc == SC_CTRL) { /* left Ctrl down                          */
    ctrl_held = true;
    return true;
  }
  if (sc == (SC_CTRL | SC_RELEASE)) { /* left Ctrl up                      */
    ctrl_held = false;
    return true;
  }
  if (sc == SC_ALT) { /* left Alt down (Alt+Tab modifier)        */
    alt_held = true;
    return true;
  }
  if (sc == (SC_ALT | SC_RELEASE)) { /* left Alt up                       */
    /* Releasing Alt after an Alt+Tab gesture commits the WM's highlighted
     * window; a bare Alt tap (no Tab) sends nothing. */
    if (alt_tabbed)
      ring_push(KEY_ALT_UP);
    alt_held = false;
    alt_tabbed = false;
    return true;
  }
  /* Alt+Tab: emit a plain Tab carrying the Alt (and any Shift) modifier and let
   * the WM decide it's a window switch - window-switching policy lives in the
   * WM, not here. alt_tabbed records that this Alt hold drove the switcher, so
   * its release commits the selection (KEY_ALT_UP). Tab is scancode 0x0F;
   * matching only the make code lets the release fall through to the generic
   * filter. */
  if (alt_held && sc == 0x0F) { /* Tab make                                */
    ring_push(KEY_TAB);
    alt_tabbed = true;
    return true;
  }
  if (sc == SC_NUMLOCK) { /* toggle Num Lock and update its LED      */
    num_lock = !num_lock;
    keyboard_set_leds();
    return true;
  }
  if (sc == SC_CAPSLOCK) { /* toggle Caps Lock and update its LED     */
    caps_lock = !caps_lock;
    keyboard_set_leds();
    return true;
  }
  return false;
}

/* Plain function keys (set 1: F1-F4 0x3B-0x3E, F7 0x41, F10 0x44, F11 0x57); F5/F6/F8/F9 are laptop media keys. */
static uint8_t fkey_code(uint8_t sc)
{
  switch (sc) {
  case 0x3B:
    return KEY_F1;
  case 0x3C:
    return KEY_F2;
  case 0x3D:
    return KEY_F3;
  case 0x3E:
    return KEY_F4;
  case 0x41:
    return KEY_F7;
  case 0x44:
    return KEY_F10;
  case 0x57:
    return KEY_F11;
  }
  return 0;
}

/* Bare F-key media codes (see SC_F*); act on the device. 1 if consumed. */
static bool process_media_fkey(uint8_t sc)
{
  switch (sc) {
  case SC_F5:
    special_key(KEY_BRIGHT_DOWN);
    return true; /* brightness down          */
  case SC_F6:
    special_key(KEY_BRIGHT_UP);
    return true; /* brightness up            */
  case SC_F8:
    special_key(KEY_VOL_DOWN);
    return true;
  case SC_F9:
    special_key(KEY_VOL_UP);
    return true;
  }
  return false;
}

/* Keypad digits, when Num Lock is on. 1 if sc was a keypad key. */
static bool process_keypad(uint8_t sc)
{
  if (sc >= SC_KP_FIRST && sc <= SC_KP_LAST) {
    if (num_lock)
      ring_push(keypad[sc - SC_KP_FIRST]);
    return true;
  }
  return false;
}

/* Super+ and Ctrl+ chords: forward the base key carrying its modifier
 * (cur_mods) and let the consumer map the chord. The driver names no chord - it
 * doesn't know "cliphist" or "save"; it just delivers key + KMOD_*. 1 if
 * consumed. */
static bool process_chords(uint8_t sc)
{
  if (super_held && active_map[sc]) {
    /* Any Super+<key>: forward the key with KMOD_SUPER; the compositor decides
     * what it means (Super+V opens clipboard history, etc.). Mark super chorded
     * so releasing Super doesn't also toggle the Start menu. */
    super_chorded = true;
    ring_push(active_map[sc]);
    return true;
  }
  if (ctrl_held) {
    char m = active_map[sc]; /* the unshifted key, layout-aware */
    /* Ctrl+C stays a kernel concern - it's a latch (not a chord code) so it can
     * interrupt a busy kernel loop that never drains the ring. Everything else
     * is the letter + KMOD_CTRL for the consumer to interpret. */
    if (m == 'c')
      int_pending = true;
    else if (m)
      ring_push(m);
    return true;
  }
  return false;
}

/* Make code -> char via the active layout; AltGr = third level, Caps Lock
 * inverts Shift for letters. */
static void emit_char(uint8_t sc)
{
  char c = 0;
  if (altgr_held && active_altgr) /* third level: AltGr punctuation  */
    c = active_altgr[sc];
  if (!c) {
    bool shift = shift_held;
    char base = active_map[sc];
    if (caps_lock && base >= 'a' && base <= 'z')
      shift = !shift;
    c = shift ? active_shift[sc] : active_map[sc];
  }
  if (c)
    ring_push(c);
}

/* One byte of the set-1 stream (0xE0 prefixes included); the caller holds kbd_lock. */
static void keyboard_decode(uint8_t sc)
{
  /* Extended keys arrive as 0xE0 then a second byte. */
  if (sc == SC_EXTENDED) {
    expecting_extended = true;
    return;
  }
  if (expecting_extended) {
    expecting_extended = false;
    process_extended(sc);
    return;
  }

  if (process_modifiers(sc))
    return;
  if (sc & SC_RELEASE) /* any other key release: ignore          */
    return;
  if (process_media_fkey(sc))
    return;
  if (sc == SC_F12) { /* the only plain F-key apps see (devtools) */
    ring_push(KEY_F12);
    return;
  }
  uint8_t fk = fkey_code(sc);
  if (fk) {
    ring_push(fk);
    return;
  }
  if (process_keypad(sc))
    return;
  if (process_chords(sc))
    return;

  emit_char(sc);
}

static void keyboard_handler(struct registers *r)
{
  (void)r;
  uint8_t sc = inb(KBD_DATA_PORT);
  uintptr_t f = spin_lock_irqsave(&kbd_lock);
  keyboard_decode(sc);
  spin_unlock_irqrestore(&kbd_lock, f);
}

void keyboard_inject_scancode(uint16_t code)
{
  uintptr_t f = spin_lock_irqsave(&kbd_lock);
  if (code & 0x100)
    keyboard_decode(SC_EXTENDED);
  keyboard_decode((uint8_t)code);
  spin_unlock_irqrestore(&kbd_lock, f);
}

void keyboard_inject_key(uint8_t key)
{
  uintptr_t f = spin_lock_irqsave(&kbd_lock);
  ring_push((char)key);
  spin_unlock_irqrestore(&kbd_lock, f);
}

uint8_t keyboard_leds(void) { return (num_lock ? 0x01 : 0) | (caps_lock ? 0x02 : 0); }

uint32_t keyboard_led_seq(void) { return led_seq; }

/* Compare an argument (terminated by NUL or a space) against a layout name. */
static bool name_eq(const char *arg, const char *name)
{
  while (*name) {
    if (*arg != *name)
      return false;
    arg++;
    name++;
  }
  return *arg == '\0' || *arg == ' ';
}

int keyboard_set_layout(const char *name)
{
  for (unsigned i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
    if (name_eq(name, layouts[i].name)) {
      active_map = layouts[i].map;
      active_shift = layouts[i].shift;
      active_altgr = layouts[i].altgr;
      active_layout = layouts[i].name;
      return 0;
    }
  }
  return -1;
}

const char *keyboard_layout_name(void) { return active_layout; }

/* --- i8042 controller bring-up -------------------------------------------- *
 * QEMU's SeaBIOS leaves the i8042 with the keyboard port enabled and the
 * keyboard-interrupt bit already set, so historically keyboard_init could get
 * away with just unmasking IRQ1. Real hardware (and UEFI firmware especially)
 * does NOT: the keyboard port and its IRQ are often left disabled, and any
 * scancode sitting unread in the output buffer blocks further IRQs. So we must
 * configure the controller ourselves here, independent of the mouse/WM path
 * (mouse_init used to be the only place this happened, but it only runs once
 * the framebuffer/window manager comes up - leaving the keyboard dead on any
 * machine where the framebuffer fails). */

#define KBD_STAT_OUTPUT_FULL 0x01 /* a byte is waiting in 0x60            */
#define KBD_STAT_INPUT_FULL 0x02  /* controller busy; don't write 0x60/64 */

static void kbd_wait_write(void)
{
  for (int i = 0; i < 100000; i++)
    if (!(inb(KBD_STATUS_PORT) & KBD_STAT_INPUT_FULL))
      return;
}

static void kbd_wait_read(void)
{
  for (int i = 0; i < 100000; i++)
    if (inb(KBD_STATUS_PORT) & KBD_STAT_OUTPUT_FULL)
      return;
}

static void keyboard_controller_init(void)
{
  /* Drain anything stale in the output buffer; a leftover byte (a scancode
   * the firmware never read) latches the i8042 and stops it raising IRQ1. */
  for (int i = 0; i < 16 && (inb(KBD_STATUS_PORT) & KBD_STAT_OUTPUT_FULL); i++)
    (void)inb(KBD_DATA_PORT);

  /* Enable the keyboard port (0xAE). Harmless if already enabled. */
  kbd_wait_write();
  outb(KBD_STATUS_PORT, 0xAE);

  /* Read the controller config byte, force the keyboard interrupt on (bit0)
   * and the keyboard clock on (clear bit4), and preserve bit6 (scancode-set-1
   * translation) so the keymaps above keep seeing set-1 codes. We OR/AND
   * explicit bits rather than trusting the read wholesale. */
  kbd_wait_write();
  outb(KBD_STATUS_PORT, 0x20); /* "read config byte"                      */
  kbd_wait_read();
  uint8_t cfg = inb(KBD_DATA_PORT);
  cfg |= 0x01;  /* bit0: keyboard IRQ (IRQ1) enabled        */
  cfg &= ~0x10; /* bit4: 0 = keyboard clock enabled         */
  kbd_wait_write();
  outb(KBD_STATUS_PORT, 0x60); /* "write config byte"                     */
  kbd_wait_write();
  outb(KBD_DATA_PORT, cfg);

  /* Tell the keyboard device itself to (re-)enable scanning. The 0xFA ACK it
   * sends back has bit7 set, so the IRQ handler treats it as a harmless
   * "release" if it lands after we unmask. */
  kbd_wait_write();
  outb(KBD_DATA_PORT, 0xF4); /* "enable scanning"                       */
  kbd_wait_read();
  (void)inb(KBD_DATA_PORT); /* swallow the ACK                          */
}

void keyboard_init(void)
{
  keyboard_controller_init(); /* configure the i8042 before any IRQ fires */
  irq_register(1, keyboard_handler);
  irq_unmask(1);
  keyboard_set_leds(); /* reflect the default Num Lock state      */
}
