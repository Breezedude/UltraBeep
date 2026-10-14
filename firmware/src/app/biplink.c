#include "biplink.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ble_common.h"
#include "ble_core.h"

#include "analog.h"
#include "backup.h"
#include "board.h"
#include "config.h"
#include "ff.h"
#include "gps.h"
#include "log.h"
#include "rtc_time.h"
#include "sensors.h"
#include "storage.h"
#include "system.h"
#include "version.h"

/*
 * File transfer protocol of the stock firmware, as captured from a BipLink session on 2026-10-10.
 * All numbers little-endian.
 *
 *   request                                  answer (notification)
 *   90 | 81 | 80          keep-alive, close, cancel          79 01 <cmd>
 *   50 00 <u16 len> path  LISTDIR                            51 01 <u16 namelen> <u32 index> <u32 count> <u32 flags>
 *                                                              <u16 fdate> <u16 ftime> <u32 size> name ... per entry,
 *                                                              then one with namelen 0 and index = count
 *   10 00 <u16 len> <u32 offset> <u32 length> path   READ    11 01 <u16 chunk> <u32 size>, then
 *                                                              12 <u8 n> <u32 offset> <n bytes> ...
 *   20 00 <u16 len> <u32 fattime> <u32 size> path    WRITE   21 10 00 00 <u32 received> <u32 chunk>
 *   22 <u8 n> <u32 offset> <n bytes>                 DATA    21 10 ... until complete, then 21 01 ...
 *
 * Status 01 = success, 10 = more data wanted; other codes are errors. Drive 2:/ is virtual:
 * app_auth_request (written, the app's 8-character secret) and app_authorized (read, one byte).
 */
#define FT_MAX        254U
#define FT_CHUNK      238U /* data bytes per READ packet (6-byte header) and per WRITE DATA packet */
#define FT_READ_MAX   248U /* the stock's "chunk" field in the READ status: ATT MTU 251 minus 3 */
#define FT_READ_BUF   (2U * FT_CHUNK) /* whole packets per refill */
#define FT_PATH_MAX   120U
#define NMEA_MAX      80U
#define ST_OK         0x01
#define ST_MORE       0x10
#define ST_ERROR      0x02
#define ST_NOT_FOUND  0x07
#define SECRET_MAX    BIPLINK_SECRET_MAX

static uint16_t dis_svc, dis_fw, dis_hw, dis_sw, dis_serial, dis_model, dis_mfr, dis_pnp;
static uint16_t bat_svc, bat_level;
static uint16_t nmea_svc, nmea_char;
static uint16_t ft_svc, ft_char, ft_desc;
static uint32_t nmea_tick;
static uint8_t battery_sent = 0xFF;

typedef enum { FT_IDLE, FT_READ, FT_WRITE } ft_state_t;

static struct {
  ft_state_t state;
  char path[FT_PATH_MAX];
  /* read */
  FIL file;
  bool file_open;
  uint32_t size, pos, end;
  uint8_t buf[FT_READ_BUF];
  uint32_t buf_len, buf_pos;
  uint32_t retry_tick;
  /* write */
  uint32_t total, received;
  bool to_secret;
  char secret[SECRET_MAX + 1];
  unsigned secret_len;
} ft;

static bool authorized;         /* this connection's app presented a known secret */
static bool config_updated;     /* the app wrote config.new.ultrabip: the system task reloads */
static bool pending;            /* a new secret waits for the button */
static bool pending_for_app;    /* ... from the app (else from the configurator's file service) */
static uint32_t pending_tick;
static char pending_secret[SECRET_MAX + 1];
static char accepted_secret[SECRET_MAX + 1]; /* confirmed in this session (kept when the list is full) */

/* ---------------------------------------------------------------- helpers */

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Sends one answer; waits a little when the stack's buffers are full (the listing bursts). */
static bool ft_send(const uint8_t *pkt, size_t len)
{
  for (int i = 0; i < 200; i++) {
    tBleStatus ret = aci_gatt_update_char_value(ft_svc, ft_char, 0, (uint8_t)len, pkt);
    if (ret == BLE_STATUS_SUCCESS) {
      return true;
    }
    if (ret != BLE_STATUS_INSUFFICIENT_RESOURCES) {
      log_printf("BipLink: send failed 0x%02X", ret);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return false;
}

static void ft_generic(uint8_t cmd)
{
  uint8_t rsp[3] = {0x79, ST_OK, cmd};
  ft_send(rsp, sizeof(rsp));
}

static void ft_read_status(uint8_t status, uint32_t size)
{
  uint8_t rsp[8] = {0x11, status};
  put16(rsp + 2, FT_READ_MAX);
  put32(rsp + 4, size);
  ft_send(rsp, sizeof(rsp));
}

static void ft_write_status(uint8_t status, uint32_t received)
{
  uint8_t rsp[12] = {0x21, status, 0, 0};
  put32(rsp + 4, received);
  put32(rsp + 8, FT_CHUNK);
  ft_send(rsp, sizeof(rsp));
}

static void ft_reset(void)
{
  if (ft.file_open) {
    storage_lock();
    f_close(&ft.file);
    storage_unlock();
    ft.file_open = false;
  }
  if (ft.state == FT_WRITE && !ft.to_secret) {
    storage_write_close();
  }
  ft.state = FT_IDLE;
}

/* ------------------------------------------------------------ authorisation */

static bool secret_known(const char *secret)
{
  const char *list = config_get()->app_secrets;
  if (!secret[0]) {
    return false;
  }
  size_t n = strlen(secret);
  for (const char *p = list; *p;) {
    const char *e = strchr(p, ',');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == n && strncmp(p, secret, n) == 0) {
      return true;
    }
    if (!e) {
      break;
    }
    p = e + 1;
  }
  return false;
}

bool biplink_secret_accepted(const char *secret)
{
  return secret_known(secret) || (secret[0] && strcmp(secret, accepted_secret) == 0);
}

void biplink_auth_request(const char *secret, bool for_app)
{
  const char *who = for_app ? "app" : "configurator";
  if (biplink_secret_accepted(secret)) {
    if (for_app) {
      authorized = true;
    }
    pending = false;
    log_printf("BipLink: known %s, authorised", who);
    return;
  }
  snprintf(pending_secret, sizeof(pending_secret), "%s", secret);
  pending = true;
  pending_for_app = for_app;
  pending_tick = HAL_GetTick();
  if (for_app) {
    authorized = false;
  }
  log_printf("BipLink: new %s asks for access - a short button press within %u s allows it", who,
             BIPLINK_AUTH_WINDOW_MS / 1000U);
}

bool biplink_auth_pending(void)
{
  if (pending && HAL_GetTick() - pending_tick > BIPLINK_AUTH_WINDOW_MS) {
    pending = false;
    log_printf("BipLink: access request not confirmed");
  }
  return pending;
}

void biplink_auth_confirm(void)
{
  if (!pending) {
    return;
  }
  pending = false;
  if (pending_for_app) {
    authorized = true;
  }
  snprintf(accepted_secret, sizeof(accepted_secret), "%s", pending_secret);
  char list[sizeof(config_get()->app_secrets)];
  const char *old = config_get()->app_secrets;
  if (strlen(old) + strlen(pending_secret) + 2U > sizeof(list)) {
    log_printf("BipLink: %s authorised for this session (the list of remembered clients is full)",
               pending_for_app ? "app" : "configurator");
    return;
  }
  strcpy(list, old); /* fits: checked above */
  if (old[0]) {
    strcat(list, ",");
  }
  strcat(list, pending_secret);
  config_save_app_secrets(list);
  log_printf("BipLink: %s authorised and remembered", pending_for_app ? "app" : "configurator");
}

/* ----------------------------------------------------------------- LISTDIR */

static bool listable(const FILINFO *fi)
{
  return fi->fname[0] && !(fi->fattrib & (AM_HID | AM_SYS));
}

static void ft_listdir(const char *path_in)
{
  DIR d;
  FILINFO fi;
  uint8_t pkt[FT_MAX];
  /* the app names folders with a trailing slash ("0:/voices/"), which f_opendir refuses */
  char path[FT_PATH_MAX];
  snprintf(path, sizeof(path), "%s", path_in);
  size_t n = strlen(path);
  while (n > 3U && path[n - 1U] == '/') {
    path[--n] = 0;
  }
  storage_lock();
  if (f_opendir(&d, path) != FR_OK) {
    storage_unlock();
    log_printf("BipLink FT: LISTDIR '%s' - no such folder", path);
    memset(pkt, 0, 24);
    pkt[0] = 0x51;
    pkt[1] = ST_NOT_FOUND;
    ft_send(pkt, 24);
    return;
  }
  uint32_t count = 0;
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
    if (listable(&fi)) {
      count++;
    }
  }
  f_rewinddir(&d);
  uint32_t index = 0;
  bool ok = true;
  while (ok && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
    if (!listable(&fi)) {
      continue;
    }
    size_t n = strlen(fi.fname);
    if (n > FT_MAX - 24U) {
      n = FT_MAX - 24U;
    }
    pkt[0] = 0x51;
    pkt[1] = ST_OK;
    put16(pkt + 2, (uint16_t)n);
    put32(pkt + 4, index++);
    put32(pkt + 8, count);
    put32(pkt + 12, (fi.fattrib & AM_DIR) ? 1U : 0U);
    put16(pkt + 16, fi.fdate);
    put16(pkt + 18, fi.ftime);
    put32(pkt + 20, (fi.fattrib & AM_DIR) ? 0U : (uint32_t)fi.fsize);
    memcpy(pkt + 24, fi.fname, n);
    storage_unlock(); /* not while a notification may wait for the stack */
    ok = ft_send(pkt, 24 + n);
    storage_lock();
  }
  f_closedir(&d);
  storage_unlock();
  memset(pkt, 0, 24);
  pkt[0] = 0x51;
  pkt[1] = ST_OK;
  put32(pkt + 4, count);
  put32(pkt + 8, count);
  if (ok) {
    ft_send(pkt, 24);
  }
  log_printf("BipLink FT: LISTDIR '%s' - %lu entries", path, (unsigned long)count);
}

/* -------------------------------------------------------------------- READ */

static void ft_read(const char *path, uint32_t offset, uint32_t length)
{
  ft_reset();
  if (strcmp(path, "2:/app_authorized") == 0) {
    uint8_t data[7] = {0x12, 1, 0, 0, 0, 0, authorized ? 1U : 0U};
    ft_read_status(ST_OK, 1);
    ft_send(data, sizeof(data));
    return;
  }
  if (strncmp(path, "2:/", 3) == 0) {
    ft_read_status(ST_NOT_FOUND, 0);
    return;
  }
  storage_lock();
  FRESULT fr = f_open(&ft.file, path, FA_READ);
  uint32_t size = fr == FR_OK ? (uint32_t)f_size(&ft.file) : 0U;
  if (fr == FR_OK && offset) {
    fr = f_lseek(&ft.file, offset < size ? offset : size);
  }
  storage_unlock();
  if (fr != FR_OK) {
    log_printf("BipLink FT: READ '%s' - not found (%d)", path, fr);
    ft_read_status(ST_NOT_FOUND, 0);
    return;
  }
  ft.file_open = true;
  ft.size = size;
  ft.pos = offset < size ? offset : size;
  ft.end = length == 0xFFFFFFFFU || ft.pos + length > size ? size : ft.pos + length;
  ft.buf_len = 0;
  ft.buf_pos = 0;
  ft.retry_tick = 0;
  ft.state = FT_READ;
  snprintf(ft.path, sizeof(ft.path), "%s", path);
  ft_read_status(ST_OK, size);
  log_printf("BipLink FT: READ '%s' %lu bytes from %lu", path, (unsigned long)(ft.end - ft.pos), (unsigned long)ft.pos);
}

/* Streams the data packets; a full stack is retried on the next tick. */
static void ft_read_tick(void)
{
  if (ft.retry_tick && HAL_GetTick() - ft.retry_tick < 3U) {
    return;
  }
  ft.retry_tick = 0;
  uint8_t pkt[6 + FT_CHUNK];
  for (int burst = 0; burst < 16 && ft.pos < ft.end; burst++) {
    if (ft.buf_pos >= ft.buf_len) {
      UINT br = 0;
      uint32_t want = ft.end - ft.pos < FT_READ_BUF ? ft.end - ft.pos : FT_READ_BUF;
      storage_lock();
      FRESULT fr = f_read(&ft.file, ft.buf, want, &br);
      storage_unlock();
      if (fr != FR_OK || br == 0) {
        log_printf("BipLink FT: read error %d", fr);
        ft_reset();
        return;
      }
      ft.buf_len = br;
      ft.buf_pos = 0;
    }
    uint32_t n = ft.buf_len - ft.buf_pos;
    if (n > FT_CHUNK) {
      n = FT_CHUNK;
    }
    pkt[0] = 0x12;
    pkt[1] = (uint8_t)n;
    put32(pkt + 2, ft.pos);
    memcpy(pkt + 6, ft.buf + ft.buf_pos, n);
    tBleStatus ret = aci_gatt_update_char_value(ft_svc, ft_char, 0, (uint8_t)(6 + n), pkt);
    if (ret == BLE_STATUS_INSUFFICIENT_RESOURCES) {
      ft.retry_tick = HAL_GetTick() | 1U;
      return;
    }
    if (ret != BLE_STATUS_SUCCESS) {
      log_printf("BipLink FT: send failed 0x%02X", ret);
      ft_reset();
      return;
    }
    ft.pos += n;
    ft.buf_pos += n;
  }
  if (ft.pos >= ft.end) {
    log_printf("BipLink FT: READ '%s' done", ft.path);
    ft_reset();
  }
}

/* ------------------------------------------------------------------- WRITE */

static void ft_write_open(const char *path, uint32_t total)
{
  ft_reset();
  ft.total = total;
  ft.received = 0;
  ft.secret_len = 0;
  ft.to_secret = strcmp(path, "2:/app_auth_request") == 0;
  snprintf(ft.path, sizeof(ft.path), "%s", path);
  if (!ft.to_secret) {
    if (strncmp(path, "0:/", 3) != 0 || !storage_write_open(path)) {
      log_printf("BipLink FT: WRITE '%s' - cannot create", path);
      ft_write_status(ST_ERROR, 0);
      return;
    }
  }
  ft.state = FT_WRITE;
  ft_write_status(ST_MORE, 0);
  log_printf("BipLink FT: WRITE '%s' %lu bytes", path, (unsigned long)total);
}

static void ft_write_data(const uint8_t *data, uint32_t n)
{
  if (ft.state != FT_WRITE) {
    ft_write_status(ST_ERROR, 0);
    return;
  }
  bool ok = true;
  if (ft.to_secret) {
    for (uint32_t i = 0; i < n && ft.secret_len < SECRET_MAX; i++) {
      ft.secret[ft.secret_len++] = (char)data[i];
    }
  } else {
    ok = storage_write_chunk(data, n);
  }
  ft.received += n;
  if (!ok) {
    ft_reset();
    ft_write_status(ST_ERROR, ft.received);
    return;
  }
  if (ft.received >= ft.total) {
    if (ft.to_secret) {
      ft.secret[ft.secret_len] = 0;
      ft.state = FT_IDLE;
      biplink_auth_request(ft.secret, true);
    } else {
      ft.state = FT_IDLE;
      bool closed = storage_write_close();
      log_printf("BipLink FT: WRITE '%s' complete (%s)", ft.path, closed ? "ok" : "close FAILED");
      if (closed && strcmp(ft.path, CONFIG_NEW_PATH) == 0) {
        config_updated = true;
      }
    }
    ft_write_status(ST_OK, ft.received);
  } else {
    ft_write_status(ST_MORE, ft.received);
  }
}

/* ----------------------------------------------------------------- requests */

static void ft_request(const uint8_t *d, uint16_t len)
{
  char path[FT_PATH_MAX];
  uint16_t path_len = len >= 4U ? get16(d + 2) : 0U;
  unsigned path_at = d[0] == 0x50 ? 4U : 12U;
  path[0] = 0;
  if ((d[0] == 0x50 || d[0] == 0x10 || d[0] == 0x20) && path_at + path_len <= len) {
    size_t n = path_len < sizeof(path) - 1U ? path_len : sizeof(path) - 1U;
    memcpy(path, d + path_at, n);
    path[n] = 0;
  }
  switch (d[0]) {
  case 0x90:
  case 0x81:
    ft_generic(d[0]);
    break;
  case 0x80:
    ft_reset();
    ft_generic(d[0]);
    break;
  case 0x50:
    ft_listdir(path);
    break;
  case 0x10:
    if (len >= 12U) {
      ft_read(path, get32(d + 4), get32(d + 8));
    }
    break;
  case 0x20:
    if (len >= 12U) {
      ft_write_open(path, get32(d + 8)); /* d[4..8]: FAT time stamp of the file, not used */
    }
    break;
  case 0x22:
    if (len >= 6U && 6U + d[1] <= len) {
      ft_write_data(d + 6, d[1]);
    }
    break;
  default:
    log_printf("BipLink FT: unknown request 0x%02X (%u bytes)", d[0], len);
    break;
  }
}

/* ---------------------------------------------------------- GATT services */

static bool add_string_char(uint16_t svc, uint16_t uuid, const char *text, uint16_t *handle)
{
  Char_UUID_t c;
  c.Char_UUID_16 = uuid;
  size_t len = strlen(text);
  if (len > 40U) {
    len = 40U;
  }
  if (aci_gatt_add_char(svc, UUID_TYPE_16, &c, 40, CHAR_PROP_READ, ATTR_PERMISSION_NONE, GATT_DONT_NOTIFY_EVENTS, 16,
                        1, handle) != BLE_STATUS_SUCCESS) {
    return false;
  }
  return aci_gatt_update_char_value(svc, *handle, 0, (uint8_t)len, (const uint8_t *)text) == BLE_STATUS_SUCCESS;
}

/* Hardware revision as the stock names it: v1.0 up to bootloader BL_v3, v1.1 from BL_v4. */
static const char *hardware_revision(char *buf, size_t size)
{
  const config_t *c = config_get();
  if (c->hw_version[0]) {
    return c->hw_version;
  }
  unsigned bl = (unsigned)((backup_bootloader_report() >> 16) & 0xFFU);
  snprintf(buf, size, "UltraBip_v1.%u+0", bl <= 3U ? 0U : 1U);
  return buf;
}

/* From the chip, as the stock does: a configuration copied from another device does not change it. */
static const char *serial_number(char *buf, size_t size)
{
  board_serial_number(buf, size);
  return buf;
}

/* The stock reports the time of its configuration file here; BipLink compares it with its copy. */
static void software_revision(char *buf, size_t size)
{
  FILINFO fi;
  storage_lock();
  FRESULT fr = f_stat(CONFIG_PATH, &fi);
  storage_unlock();
  if (fr != FR_OK) {
    snprintf(buf, size, "0");
    return;
  }
  uint32_t t = unix_from_civil((uint16_t)(1980U + (fi.fdate >> 9)), (uint8_t)((fi.fdate >> 5) & 0xFU),
                               (uint8_t)(fi.fdate & 0x1FU), (uint8_t)(fi.ftime >> 11), (uint8_t)((fi.ftime >> 5) & 0x3FU),
                               (uint8_t)((fi.ftime & 0x1FU) * 2U));
  snprintf(buf, size, "%lu", (unsigned long)t);
}

bool biplink_add_services(void)
{
  Service_UUID_t s;
  Char_UUID_t c;
  Char_Desc_Uuid_t du;
  char buf[48];
  const config_t *cfg = config_get();

  s.Service_UUID_16 = 0x180A;
  if (aci_gatt_add_service(UUID_TYPE_16, &s, PRIMARY_SERVICE, 16, &dis_svc) != BLE_STATUS_SUCCESS) {
    return false;
  }
  bool ok = add_string_char(dis_svc, 0x2A29, "Stodeus", &dis_mfr);
  ok = ok && add_string_char(dis_svc, 0x2A24, "Daedalus", &dis_model); /* the stock's model name */
  ok = ok && add_string_char(dis_svc, 0x2A25, serial_number(buf, sizeof(buf)), &dis_serial);
  ok = ok && add_string_char(dis_svc, 0x2A27, hardware_revision(buf, sizeof(buf)), &dis_hw);
  ok = ok && add_string_char(dis_svc, 0x2A26, cfg->stock_fw_version[0] ? cfg->stock_fw_version : BIPLINK_FIRMWARE_VERSION,
                             &dis_fw);
  software_revision(buf, sizeof(buf));
  ok = ok && add_string_char(dis_svc, 0x2A28, buf, &dis_sw);
  static const uint8_t pnp[7] = {0x01, 0x57, 0x0D, 0x00, 0x01, 0x01, 0x00}; /* as the stock: SIG vendor 0x0D57 */
  c.Char_UUID_16 = 0x2A50;
  ok = ok && aci_gatt_add_char(dis_svc, UUID_TYPE_16, &c, 7, CHAR_PROP_READ, ATTR_PERMISSION_NONE, GATT_DONT_NOTIFY_EVENTS,
                               16, 0, &dis_pnp) == BLE_STATUS_SUCCESS;
  ok = ok && aci_gatt_update_char_value(dis_svc, dis_pnp, 0, 7, pnp) == BLE_STATUS_SUCCESS;

  s.Service_UUID_16 = 0x180F;
  ok = ok && aci_gatt_add_service(UUID_TYPE_16, &s, PRIMARY_SERVICE, 4, &bat_svc) == BLE_STATUS_SUCCESS;
  c.Char_UUID_16 = 0x2A19;
  ok = ok && aci_gatt_add_char(bat_svc, UUID_TYPE_16, &c, 1, CHAR_PROP_READ | CHAR_PROP_NOTIFY, ATTR_PERMISSION_NONE,
                               GATT_DONT_NOTIFY_EVENTS, 16, 0, &bat_level) == BLE_STATUS_SUCCESS;

  s.Service_UUID_16 = 0xFFE0;
  ok = ok && aci_gatt_add_service(UUID_TYPE_16, &s, PRIMARY_SERVICE, 4, &nmea_svc) == BLE_STATUS_SUCCESS;
  c.Char_UUID_16 = 0xFFE1;
  ok = ok && aci_gatt_add_char(nmea_svc, UUID_TYPE_16, &c, NMEA_MAX, CHAR_PROP_READ | CHAR_PROP_NOTIFY,
                               ATTR_PERMISSION_NONE, GATT_DONT_NOTIFY_EVENTS, 16, 1, &nmea_char) == BLE_STATUS_SUCCESS;

  s.Service_UUID_16 = 0x570D;
  ok = ok && aci_gatt_add_service(UUID_TYPE_16, &s, PRIMARY_SERVICE, 6, &ft_svc) == BLE_STATUS_SUCCESS;
  c.Char_UUID_16 = 0x0200;
  ok = ok && aci_gatt_add_char(ft_svc, UUID_TYPE_16, &c, FT_MAX, CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_NOTIFY,
                               ATTR_PERMISSION_NONE, GATT_NOTIFY_ATTRIBUTE_WRITE, 16, 1, &ft_char) == BLE_STATUS_SUCCESS;
  du.Char_UUID_16 = 0x2901; /* user description, read by the app */
  ok = ok && aci_gatt_add_char_desc(ft_svc, ft_char, UUID_TYPE_16, &du, 12, 12, (const uint8_t *)"FileTransfer",
                                    ATTR_PERMISSION_NONE, ATTR_ACCESS_READ_ONLY, GATT_DONT_NOTIFY_EVENTS, 16, 0,
                                    &ft_desc) == BLE_STATUS_SUCCESS;
  if (!ok) {
    log_printf("BLE: stock services failed");
  }
  return ok;
}

void biplink_on_connect(void)
{
  char buf[24];
  software_revision(buf, sizeof(buf));
  aci_gatt_update_char_value(dis_svc, dis_sw, 0, (uint8_t)strlen(buf), (const uint8_t *)buf);
  battery_sent = 0xFF; /* a new link gets the level again; the app's session on the other link goes on */
}

void biplink_on_disconnect(void)
{
  authorized = false;
  ft_reset();
}

bool biplink_on_attribute_modified(uint16_t attr_handle, const uint8_t *data, uint16_t len)
{
  if (ft_char && attr_handle == ft_char + 1U) {
    if (len) {
      ft_request(data, len);
    }
    return true;
  }
  return false;
}

bool biplink_take_config_update(void)
{
  bool v = config_updated;
  config_updated = false;
  return v;
}

bool biplink_busy(void)
{
  return ft.state == FT_READ;
}

/* ------------------------------------------------------------- NMEA, battery */

static uint8_t nmea_checksum(const char *s)
{
  uint8_t cs = 0;
  for (const char *p = s + 1; *p && *p != '*'; p++) {
    cs ^= (uint8_t)*p;
  }
  return cs;
}

/* Appends the checksum and notifies the line. */
static void notify_nmea(char *line, int n, size_t size)
{
  if (n < 0 || (size_t)n + 4U >= size) {
    return;
  }
  n += snprintf(line + n, size - (size_t)n, "%02X\r\n", nmea_checksum(line));
  aci_gatt_update_char_value(nmea_svc, nmea_char, 0, (uint8_t)n, (const uint8_t *)line);
}

/*
 * $LXWP0 as the stock writes it: logger flag N, no airspeed, altitude m (one decimal), one vario
 * value m/s (two decimals), the five further vario fields empty, heading in degrees (ground track,
 * only while moving with a fix), no wind. Unknown values stay empty.
 */
static void send_lxwp0(const sensors_t *sn)
{
  char alt[16] = "", vario[16] = "", heading[8] = "";
  if (sn->valid) {
    snprintf(alt, sizeof(alt), "%.1f", (double)sn->alt_m);
    snprintf(vario, sizeof(vario), "%.2f", (double)sn->vario_ms);
  }
  gps_status_t g;
  gps_get(&g);
  if (g.state == GPS_FIX && g.speed_kmh >= 3.0f) {
    snprintf(heading, sizeof(heading), "%ld", (long)(g.course_deg + 0.5f) % 360L);
  }
  char line[NMEA_MAX];
  int n = snprintf(line, sizeof(line), "$LXWP0,N,,%s,%s,,,,,,%s,,*", alt, vario, heading);
  notify_nmea(line, n, sizeof(line));
}

/* $LK8EX1,pressure Pa,altitude m,vario cm/s,temperature C,1000 + battery % */
static void send_nmea(void)
{
  sensors_t sn;
  sensors_get(&sn);
  uint32_t mv = system_battery_mv();
  long batt = mv ? 1000L + (long)analog_battery_percent(mv) : 999L;
  char line[NMEA_MAX];
  int n = snprintf(line, sizeof(line), "$LK8EX1,%ld,%ld,%ld,%ld,%ld,*", sn.valid ? (long)(sn.pressure_pa + 0.5f) : 999999L,
                   sn.valid ? (long)(sn.alt_m + (sn.alt_m < 0 ? -0.5f : 0.5f)) : 99999L,
                   sn.valid ? (long)(sn.vario_ms * 100.0f + (sn.vario_ms < 0 ? -0.5f : 0.5f)) : 0L,
                   sn.valid ? (long)(sn.temp_c + 0.5f) : 99L, batt);
  notify_nmea(line, n, sizeof(line));
  send_lxwp0(&sn);
}

void biplink_tick(void)
{
  if (ft.state == FT_READ) {
    ft_read_tick();
  }
  if (!nmea_char || HAL_GetTick() - nmea_tick < 1000U) {
    return;
  }
  nmea_tick = HAL_GetTick();
  send_nmea();
  uint32_t mv = system_battery_mv();
  if (mv) {
    uint8_t pct = (uint8_t)analog_battery_percent(mv);
    if (pct != battery_sent) {
      battery_sent = pct;
      aci_gatt_update_char_value(bat_svc, bat_level, 0, 1, &pct);
    }
  }
}
