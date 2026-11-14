/* nodev.c: the PC devices the shared kernel can drive (IDE, AHCI, NVMe, SCSI and SD storage, HDA, SB16 and ES1371
 * audio, the PS/2 mouse, the x86 NICs, the VBE framebuffer, PCI, ACPI, USB, Bluetooth, and the VMware and SPICE
 * clipboards), as seen on a machine that has none of them: each reports itself absent the way its x86 driver does
 * when the probe finds nothing, and its I/O fails. */
#include "ahci.h"
#include "ata.h"
#include "atapi.h"
#include "backlight.h"
#include "battery.h"
#include "bootmod.h"
#include "bt.h"
#include "console.h"
#include "edid.h"
#include "es1371.h"
#include "fb.h"
#include "hda.h"
#include "hid.h"
#include "mouse.h"
#include "nic.h"
#include "nvme.h"
#include "pci.h"
#include "sb16.h"
#include "scsi.h"
#include "sd.h"
#include "ufs.h"
#include "usb.h"
#include "uvc.h"
#include "vdagent.h"
#include "vmware.h"
#include "xhci.h"

/* Store an empty string, for the queries that fill a caller's buffer. */
static int empty_text(char *buf, int max)
{
  if (buf && max > 0)
    buf[0] = '\0';
  return 0;
}

int ata_present(void) { return 0; }
void ata_dma_init(void) {}
uint32_t ata_sectors(void) { return 0; }
const char *ata_model(void) { return ""; }
int ata_read_sector(uint32_t lba, void *buf) { return ata_read(lba, buf, 1); }
int ata_write_sector(uint32_t lba, const void *buf) { return ata_write(lba, buf, 1); }
int ata_read(uint32_t lba, void *buf, uint32_t count)
{
  (void)lba, (void)buf, (void)count;
  return -1;
}
int ata_write(uint32_t lba, const void *buf, uint32_t count)
{
  (void)lba, (void)buf, (void)count;
  return -1;
}

int atapi_present(void) { return 0; }
const char *atapi_model(void) { return ""; }
int atapi_read_sector(uint32_t lba, void *buf)
{
  (void)lba, (void)buf;
  return -1;
}

int ahci_present(void) { return 0; }
uint32_t ahci_sectors(void) { return 0; }
const char *ahci_model(void) { return ""; }
int ahci_read_sector(uint32_t lba, void *buf) { return ahci_read(lba, buf, 1); }
int ahci_write_sector(uint32_t lba, const void *buf) { return ahci_write(lba, buf, 1); }
int ahci_read(uint32_t lba, void *buf, uint32_t count)
{
  (void)lba, (void)buf, (void)count;
  return -1;
}
int ahci_write(uint32_t lba, const void *buf, uint32_t count)
{
  (void)lba, (void)buf, (void)count;
  return -1;
}

int nvme_present(void) { return 0; }
uint32_t nvme_sectors(void) { return 0; }
const char *nvme_model(void) { return ""; }
int nvme_read_sector(uint32_t lba, void *buf) { return nvme_read(lba, buf, 1); }
int nvme_write_sector(uint32_t lba, const void *buf) { return nvme_write(lba, buf, 1); }
int nvme_read(uint32_t lba, void *buf, uint32_t count)
{
  (void)lba, (void)buf, (void)count;
  return -1;
}
int nvme_write(uint32_t lba, const void *buf, uint32_t count)
{
  (void)lba, (void)buf, (void)count;
  return -1;
}

int scsi_present(void) { return 0; }
uint32_t scsi_sectors(void) { return 0; }
const char *scsi_model(void) { return ""; }
const char *scsi_hba_name(void) { return ""; }
int scsi_read_sector(uint32_t lba, void *buf)
{
  (void)lba, (void)buf;
  return -1;
}
int scsi_write_sector(uint32_t lba, const void *buf)
{
  (void)lba, (void)buf;
  return -1;
}

int sd_present(void) { return 0; }
uint32_t sd_sectors(void) { return 0; }
const char *sd_model(void) { return ""; }
int sd_read_sector(uint32_t lba, void *buf)
{
  (void)lba, (void)buf;
  return -1;
}
int sd_write_sector(uint32_t lba, const void *buf)
{
  (void)lba, (void)buf;
  return -1;
}

int ufs_present(void) { return 0; }
uint32_t ufs_sectors(void) { return 0; }
const char *ufs_model(void) { return ""; }
uint8_t ufs_lun(void) { return 0; }
int ufs_read_sector(uint32_t lba, void *buf)
{
  (void)lba, (void)buf;
  return -1;
}
int ufs_write_sector(uint32_t lba, const void *buf)
{
  (void)lba, (void)buf;
  return -1;
}

int xhci_msc_present(void) { return 0; }
uint32_t xhci_msc_sectors(void) { return 0; }
const char *xhci_msc_model(void) { return ""; }
int xhci_msc_read(uint32_t lba, void *buf)
{
  (void)lba, (void)buf;
  return -1;
}
int xhci_msc_write(uint32_t lba, const void *buf)
{
  (void)lba, (void)buf;
  return -1;
}

const void *bootmod_find(const char *name, uint32_t *len)
{
  (void)name;
  if (len)
    *len = 0;
  return 0;
}

int pci_dev_info(int idx, struct pci_dev_info *out)
{
  (void)idx, (void)out;
  return 0;
}

void usb_list(int detail_port)
{
  (void)detail_port;
  con_write("No USB controllers found.\n");
}

void nic_init(void) { con_write("net: no NIC found\n"); }
int nic_present(void) { return 0; }
const uint8_t *nic_mac(void) { return 0; }
void nic_send(const uint8_t *frame, uint32_t len) { (void)frame, (void)len; }
void nic_poll(void) {}

bool hda_present(void) { return false; }
int hda_stream_open(void) { return -1; }
void hda_stream_close(void) {}
int hda_stream_write(const void *pcm, uint32_t bytes)
{
  (void)pcm, (void)bytes;
  return -1;
}
void hda_volume_step(int dir) { (void)dir; }
void hda_volume_mute(void) {}
int hda_volume_level(void) { return 0; }
int hda_is_muted(void) { return 0; }
int hda_output_count(void) { return 0; }
int hda_output_active(void) { return -1; }
int hda_output_select(int i)
{
  (void)i;
  return -1;
}
int hda_output_name(int i, char *out, int max)
{
  (void)i, (void)out, (void)max;
  return 0;
}
bool hda_capture_present(void) { return false; }
bool hda_capture_active(void) { return false; }
int hda_capture_start(void) { return -1; }
int hda_capture_stop(void) { return -1; }
uint32_t hda_capture_pos(void) { return 0; }
int hda_capture_read(uint32_t off, void *buf, uint32_t len)
{
  (void)off, (void)buf, (void)len;
  return 0;
}

bool sb16_present(void) { return false; }
int sb16_stream_open(void) { return -1; }
void sb16_stream_close(void) {}
int sb16_stream_write(const void *pcm, uint32_t bytes)
{
  (void)pcm, (void)bytes;
  return -1;
}
void sb16_volume_step(int dir) { (void)dir; }
void sb16_volume_mute(void) {}
int sb16_volume_level(void) { return 0; }
int sb16_is_muted(void) { return 0; }

bool es1371_present(void) { return false; }
int es1371_stream_open(void) { return -1; }
void es1371_stream_close(void) {}
int es1371_stream_write(const void *pcm, uint32_t bytes)
{
  (void)pcm, (void)bytes;
  return -1;
}
void es1371_volume_step(int dir) { (void)dir; }
void es1371_volume_mute(void) {}
int es1371_volume_level(void) { return 0; }
int es1371_is_muted(void) { return 0; }

bool bt_present(void) { return false; }
int bt_command(const char *line)
{
  (void)line;
  return -1;
}
int bt_status_text(char *buf, int max) { return empty_text(buf, max); }
int bt_devices_text(char *buf, int max) { return empty_text(buf, max); }
int bt_le_devices_text(char *buf, int max) { return empty_text(buf, max); }
int bt_gatt_text(char *buf, int max) { return empty_text(buf, max); }
int bt_gatt_events_text(char *buf, int max) { return empty_text(buf, max); }
int bt_a2dp_open(void) { return -1; }
void bt_a2dp_close(void) {}
bool bt_a2dp_ready(void) { return false; }
int bt_a2dp_write(const void *pcm, uint32_t bytes)
{
  (void)pcm, (void)bytes;
  return -1;
}

bool uvc_present(void) { return false; }
int uvc_command(const char *line)
{
  (void)line;
  return -1;
}
int uvc_info_text(char *buf, int max) { return empty_text(buf, max); }
int uvc_control_text(char *buf, int max) { return empty_text(buf, max); }
uint32_t uvc_frame_size(void) { return 0; }
int uvc_frame_read(uint32_t off, void *buf, uint32_t len)
{
  (void)off, (void)buf, (void)len;
  return 0;
}

bool hid_gamepad_can_rumble(void) { return false; }
int hid_gamepad_id(char *buf, int max)
{
  (void)buf, (void)max;
  return -1;
}
int hid_gamepad_state(char *buf, int max)
{
  (void)buf, (void)max;
  return -1;
}
int hid_gamepad_rumble(uint16_t strong, uint16_t weak, uint32_t ms)
{
  (void)strong, (void)weak, (void)ms;
  return -1;
}

int mouse_x(void) { return 0; }
int mouse_y(void) { return 0; }
int mouse_buttons(void) { return 0; }
int mouse_wheel(void) { return 0; }
void mouse_set_bounds(int max_x, int max_y) { (void)max_x, (void)max_y; }
void mouse_set_speed(int level) { (void)level; }
int mouse_speed(void) { return 0; }

/* The boot framebuffer, when there is one, belongs to fbcon; there is no graphics mode for the compositor. */
int fb_active(void) { return 0; }
uint32_t fb_width(void) { return 0; }
uint32_t fb_height(void) { return 0; }
void fb_dims(int *w, int *h, int *pitch, int *bpp)
{
  if (w)
    *w = 0;
  if (h)
    *h = 0;
  if (pitch)
    *pitch = 0;
  if (bpp)
    *bpp = 0;
}
uint32_t fb_backbuffer_phys(void) { return 0; }
uint32_t fb_backbuffer_bytes(void) { return 0; }
void fb_present_rects(const struct fb_rect *r, int n) { (void)r, (void)n; }
void fb_con_putc(char c) { (void)c; }
int fb_cursor_set(const uint32_t *argb, int w, int h, int hot_x, int hot_y, int scale)
{
  (void)argb, (void)w, (void)h, (void)hot_x, (void)hot_y, (void)scale;
  return -1;
}
void fb_cursor_move(int x, int y) { (void)x, (void)y; }
int fb_can_set_mode(void) { return 0; }
int fb_set_mode(uint32_t w, uint32_t h)
{
  (void)w, (void)h;
  return -1;
}
int fb_mode_get(int i, uint32_t *w, uint32_t *h)
{
  (void)i, (void)w, (void)h;
  return 0;
}
void fb_poll_resize(void) {}
int fb_target_hz(void) { return 0; }
void fb_set_target_hz(int hz) { (void)hz; }
int fb_ui_scale(void) { return 0; }
void fb_set_ui_scale(int s) { (void)s; }
int fb_ui_scale_active(void) { return 1; }
int fb_brightness_percent(void) { return 0; }
void fb_set_brightness_percent(int percent) { (void)percent; }

bool backlight_present(void) { return false; }
uint32_t backlight_max(void) { return 0; }
uint32_t backlight_raw(void) { return 0; }
uint32_t backlight_actual(void) { return 0; }
void backlight_set_raw(uint32_t duty) { (void)duty; }

void edid_rescan(void) {}
int edid_connected(void) { return 0; }
int edid_internal(void) { return 0; }
int edid_hdmi(void) { return 0; }
int edid_hdmi_audio(void) { return 0; }
int edid_dpi(void) { return 0; }
int edid_refresh_hz(void) { return 0; }
void edid_model(char *out, int len) { empty_text(out, len); }

int battery_present(void) { return 0; }
int battery_percent(int *charging, int *discharging, int *minutes)
{
  if (charging)
    *charging = 0;
  if (discharging)
    *discharging = 0;
  if (minutes)
    *minutes = -1;
  return -1;
}
const char *battery_diag(void) { return "no ACPI battery on this machine"; }

int vmware_present(void) { return 0; }
int vmware_clip_send(const char *buf, int len)
{
  (void)buf, (void)len;
  return -1;
}
int vmware_clip_recv(char *buf, int max)
{
  (void)buf, (void)max;
  return -1;
}

int vdagent_present(void) { return 0; }
int vdagent_clip_send(const char *buf, int len)
{
  (void)buf, (void)len;
  return -1;
}
int vdagent_clip_recv(char *buf, int max)
{
  (void)buf, (void)max;
  return 0;
}
