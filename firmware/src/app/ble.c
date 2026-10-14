/*
 * Bluetooth Low Energy: radio core start-up, GAP/GATT and the file service
 * of the configurator (docs/ble-file-service.md).
 *
 * Two links at once (the phone's app and the configurator): the device keeps
 * advertising while one central is connected, on the ground only. Each
 * service belongs to the link that uses it: the stock file transfer to the
 * app's, the file service and the console to the configurator's (one at a
 * time), the remote's key reports to the phone that enabled them. The file
 * service takes commands only after an AUTH with a secret the pilot confirmed
 * by a button press once (the BipLink mechanism, biplink.h).
 *
 * CPU2 runs the wireless stack installed on the device; it is booted, never
 * modified. The BLE host is started with the parameter block of the stock
 * firmware, which fits that stack version. Interrupts (IPCC) only queue events
 * and wake the BLE task; every HCI command runs in the BLE task. A command
 * that CPU2 never answers stops BLE for the session instead of hanging.
 */
#include "ble_app.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app_common.h"
#include "ble_common.h"
#include "ble_core.h"
#include "hci_tl.h"
#include "hw.h"
#include "shci.h"
#include "shci_tl.h"
#include "stm32wbxx_ll_hsem.h"
#include "tl.h"

#include "audio.h"
#include "battery.h"
#include "biplink.h"
#include "ble_console.h"
#include "board.h"
#include "config.h"
#include "crc32.h"
#include "ff.h"
#include "health.h"
#include "identity.h"
#include "remote.h"
#include "gps.h"
#include "log.h"
#include "recorder.h"
#include "sensors.h"
#include "sound.h"
#include "storage.h"
#include "system.h"
#include "vario.h"
#include "vario_audio.h"
#include "version.h"

/* ------------------------------------------------------------------ mailbox */

#define POOL_SIZE (CFG_TLBLE_EVT_QUEUE_LENGTH * 4U * DIVC((sizeof(TL_PacketHeader_t) + TL_BLE_EVENT_FRAME_SIZE), 4U))

PLACE_IN_SECTION("MB_MEM2") ALIGN(4) static uint8_t evt_pool[POOL_SIZE];
PLACE_IN_SECTION("MB_MEM2") ALIGN(4) static TL_CmdPacket_t system_cmd_buffer;
PLACE_IN_SECTION("MB_MEM2") ALIGN(4) static uint8_t system_spare_evt_buffer[sizeof(TL_PacketHeader_t) + TL_EVT_HDR_SIZE + 255U];
PLACE_IN_SECTION("MB_MEM2") ALIGN(4) static uint8_t ble_spare_evt_buffer[sizeof(TL_PacketHeader_t) + TL_EVT_HDR_SIZE + 255U];
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static TL_CmdPacket_t ble_cmd_buffer;

/* ------------------------------------------------- BLE init parameters */

typedef struct __attribute__((packed)) {
  uint32_t ble_buffer_address;
  uint32_t ble_buffer_size;
  uint16_t num_attr_record;
  uint16_t num_attr_serv;
  uint16_t attr_value_arr_size;
  uint8_t num_of_links;
  uint8_t extended_packet_length_enable;
  uint8_t pr_write_list_size;
  uint8_t mblock_count;
  uint16_t att_mtu;
  uint16_t peripheral_sca;
  uint8_t central_sca;
  uint8_t ls_source;
  uint32_t max_conn_event_length;
  uint16_t hs_startup_time;
  uint8_t viterbi_enable;
  uint8_t options;
  uint8_t hw_version;
  uint8_t max_coc_initiator_nbr;
  int8_t min_tx_power;
  int8_t max_tx_power;
  uint8_t rx_model_config;
  uint8_t max_adv_set_nbr;
  uint16_t max_adv_data_len;
  int16_t tx_path_compens;
  int16_t rx_path_compens;
} ble_init_params_t;

_Static_assert(sizeof(ble_init_params_t) == 44, "SHCI_C2_BLE_Init payload of the installed stack is 44 bytes");

/* Same values as the stock firmware: they match the installed stack (BLE light 1.14). */
static ble_init_params_t init_params = {
  .num_attr_record = 100, /* the stock's 68 plus our file service (the stock services are added too, biplink.c) */
  .num_attr_serv = 12,
  .attr_value_arr_size = 1900,
  .num_of_links = 2, /* the app and the configurator; the installed light stack allows two */
  .extended_packet_length_enable = 1,
  .pr_write_list_size = 28,
  .mblock_count = 58, /* BLE_MBLOCKS_CALC(28, 251, 2 links) */
  .att_mtu = 251,
  .peripheral_sca = 500,
  .central_sca = 0,
  .ls_source = 1,
  .max_conn_event_length = 0xFFFFFFFFU,
  .hs_startup_time = 0x148,
  .viterbi_enable = 1,
  .options = 0,
  .hw_version = 0,
  .max_coc_initiator_nbr = 32,
  .min_tx_power = -40,
  .max_tx_power = 6,
  .rx_model_config = 0,
  .max_adv_set_nbr = 3,
  .max_adv_data_len = 1650,
  .tx_path_compens = 0,
  .rx_path_compens = 0,
};

/* ------------------------------------------------------------------- state */

#define TX_POWER_LEVEL 0x18 /* about 0 dBm */
/* Advertising: 100-200 ms for this long after the start or a disconnect, then 800-1000 ms (less current;
 * a phone or the configurator still finds the device within a few seconds). */
#define ADV_FAST_MS 30000U
#define VALUE_MAX      244  /* ATT MTU 247 - 3 */
#define XFER_MAX       (24U * 1024U) /* config files, voice clips (about 9 kB per second) */
#define PUT_TIMEOUT_MS 10000U
/* PUT with "ack": the client writes without response and keeps at most PUT_WINDOW bytes beyond
 * the last "ACK <received>" in flight; the device acknowledges every ACK_STEP bytes. */
#define ACK_STEP       1024U
#define PUT_WINDOW     4096U
/* A streamed image is written to the card in pieces of this size (whole sectors, one multi-block command). */
#define FILE_FLUSH     4096U
/* Asks the central for a 7.5-15 ms connection interval this long after connecting. */
#define CONN_UPDATE_DELAY_MS 1500U

/* 4a8000xx-c6ce-4362-96ab-eb24df2be84a, little endian */
#define FS_UUID(x) {0x4A, 0xE8, 0x2B, 0xDF, 0x24, 0xEB, 0xAB, 0x96, 0x62, 0x43, 0xCE, 0xC6, (x), 0x00, 0x80, 0x4A}

/* XF_GET: a file or listing held in xfer_buf; XF_STREAM: a track file read from the card in chunks. */
typedef enum { XF_IDLE, XF_GET, XF_PUT, XF_STREAM, XF_PUT_FILE } xfer_t;

/* A firmware image is streamed to the card (too large for xfer_buf): "PUT update <size> <crc>", then "UPDATE". */
#define UPDATE_PATH     "0:/ultrabip_ultrabeep.update" /* the bootloader takes ultrabip*.update only */
#define UPDATE_TMP_PATH "0:/ultrabip_ultrabeep.tmp" /* not an update candidate until renamed */
#define UPDATE_MAX      (400U * 1024U)
#define UPDATE_MIN_MV   BATTERY_START_MV /* below it the device would not switch on afterwards */
static uint32_t xf_run_crc; /* XF_PUT_FILE: the CRC of what arrived */
static uint32_t put_buffered; /* XF_PUT_FILE: bytes in xfer_buf not yet written to the card */
static bool xf_ack;           /* the client asked for ACK lines */
static uint32_t xf_start;     /* tick of READY, for the log */
static void put_file_abort(void);
static void put_ready(bool ack);
#define STREAM_CHUNK 4096U

static TaskHandle_t task;
static volatile bool sys_evt_pending, hci_evt_pending;
static volatile bool shci_cmd_done, hci_cmd_done;
static volatile bool sys_ready;
static volatile bool want_on;
static volatile bool cpu2_started;
static jmp_buf abort_jmp;
static bool abort_armed;
static bool dead;
static bool host_ready;
static bool advertising;
static bool adv_slow;      /* the slow interval is in use */
static uint32_t adv_tick;  /* when the current advertising started */

static ble_info_t info;
static char dev_name[IDENTITY_NAME_MAX + 1];
static uint8_t own_addr[6];
static uint16_t fs_svc, fs_cmd, fs_data, fs_status;
static volatile bool tx_pool_free = true;

/* The connected centrals. */
#define LINKS_MAX 2
#define NO_LINK   0xFFFFU
#define ADV_RETRY_MS 5000U /* after advertising failed while connected */
typedef struct {
  uint16_t handle; /* NO_LINK: free */
  uint16_t mtu;
  uint16_t interval;
  uint8_t phy;
  uint32_t tick;      /* when it connected */
  bool update_sent;   /* the connection parameter request went out */
  bool fs_authorized; /* may use the file service (AUTH) */
  bool fs_auth_wait;  /* its AUTH waits for the button */
  char fs_secret[BIPLINK_SECRET_MAX + 1];
} link_t;
static link_t links[LINKS_MAX] = {{.handle = NO_LINK}, {.handle = NO_LINK}};
static uint16_t fs_link = NO_LINK;     /* the link using the file service and the console (one at a time) */
static uint16_t bl_link = NO_LINK;     /* the link using the stock file transfer (the BipLink app) */
static uint16_t remote_link = NO_LINK; /* the link that wrote to the remote's HID service */
static uint32_t adv_fail_tick;
static bool adv_fail_logged; /* one log line per run of failures (retried every ADV_RETRY_MS) */

static link_t *link_by(uint16_t handle)
{
  for (unsigned i = 0; i < LINKS_MAX; i++) {
    if (links[i].handle == handle && handle != NO_LINK) {
      return &links[i];
    }
  }
  return NULL;
}

static link_t *link_free(void)
{
  for (unsigned i = 0; i < LINKS_MAX; i++) {
    if (links[i].handle == NO_LINK) {
      return &links[i];
    }
  }
  return NULL;
}

static unsigned link_count(void)
{
  unsigned n = 0;
  for (unsigned i = 0; i < LINKS_MAX; i++) {
    n += links[i].handle != NO_LINK;
  }
  return n;
}

/* The link the INFO values describe: the configurator's, else the first. */
static const link_t *info_link(void)
{
  const link_t *l = link_by(fs_link);
  for (unsigned i = 0; !l && i < LINKS_MAX; i++) {
    if (links[i].handle != NO_LINK) {
      l = &links[i];
    }
  }
  return l;
}

static void info_refresh(void)
{
  const link_t *l = info_link();
  info.links = (uint8_t)link_count();
  info.connected = info.links > 0;
  info.mtu = l ? l->mtu : 23;
  info.interval = l ? l->interval : 0;
  info.phy = l ? l->phy : 1;
}
static uint32_t tx_pool_full_tick;

static xfer_t xf;
static char xfer_buf[XFER_MAX + 1];
static uint32_t xf_size, xf_pos, xf_crc, xf_tick;
static char xf_path[72];
static FIL xf_file;
static bool xf_file_open;
static uint32_t xf_buf_len, xf_buf_pos; /* XF_STREAM: the chunk in xfer_buf */

/* ------------------------------------------------------------- helpers */

static void ble_abort(const char *why)
{
  log_printf("BLE: %s - Bluetooth off for this session", why);
  dead = true;
  host_ready = false;
  info.running = false;
  if (abort_armed) {
    longjmp(abort_jmp, 1);
  }
}

static void wake(void)
{
  if (!task) {
    return;
  }
  if (__get_IPSR()) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &woken);
    portYIELD_FROM_ISR(woken);
  } else {
    xTaskNotifyGive(task);
  }
}

/* --------------------------------------- transport layer callbacks */

void shci_notify_asynch_evt(void *pdata)
{
  (void)pdata;
  sys_evt_pending = true;
  wake();
}

void shci_cmd_resp_release(uint32_t flag)
{
  (void)flag;
  shci_cmd_done = true;
  wake();
}

void shci_cmd_resp_wait(uint32_t timeout)
{
  (void)timeout;
  uint32_t t0 = HAL_GetTick();
  while (!shci_cmd_done) {
    health_alive(HEALTH_BLE);
    if (HAL_GetTick() - t0 > 3000U) {
      ble_abort("system command timeout");
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
  }
}

static void shci_status(SHCI_TL_CmdStatus_t status)
{
  if (status == SHCI_TL_CmdBusy) {
    shci_cmd_done = false;
  }
}

void hci_notify_asynch_evt(void *pdata)
{
  (void)pdata;
  hci_evt_pending = true;
  wake();
}

void hci_cmd_resp_release(uint32_t flag)
{
  (void)flag;
  hci_cmd_done = true;
  wake();
}

void hci_cmd_resp_wait(uint32_t timeout)
{
  (void)timeout;
  uint32_t t0 = HAL_GetTick();
  while (!hci_cmd_done) {
    health_alive(HEALTH_BLE);
    if (HAL_GetTick() - t0 > 3000U) {
      ble_abort("HCI command timeout");
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
  }
  hci_cmd_done = false;
}

static void hci_status(HCI_TL_CmdStatus_t status)
{
  if (status == HCI_TL_CmdBusy) {
    hci_cmd_done = false;
  }
}

void IPCC_C1_RX_IRQHandler(void)
{
  HW_IPCC_Rx_Handler();
}

void IPCC_C1_TX_IRQHandler(void)
{
  HW_IPCC_Tx_Handler();
}

/* ----------------------------------------------------- system events */

static void sys_user_evt(void *payload)
{
  tSHCI_UserEvtRxParam *param = (tSHCI_UserEvtRxParam *)payload;
  TL_AsynchEvt_t *evt = (TL_AsynchEvt_t *)param->pckt->evtserial.evt.payload;
  if (evt->subevtcode == SHCI_SUB_EVT_CODE_READY) {
    SHCI_C2_Ready_Evt_t *ready = (SHCI_C2_Ready_Evt_t *)evt->payload;
    WirelessFwInfo_t fw;
    SHCI_GetWirelessFwInfo(&fw);
    info.fw_major = fw.VersionMajor;
    info.fw_minor = fw.VersionMinor;
    info.fw_sub = fw.VersionSub;
    info.fus_major = fw.FusVersionMajor;
    info.fus_minor = fw.FusVersionMinor;
    info.fus_sub = fw.FusVersionSub;
    info.stack_type = fw.StackType;
    if (ready->sysevt_ready_rsp != WIRELESS_FW_RUNNING) {
      param->status = SHCI_TL_UserEventFlow_Disable; /* the FUS cannot release it */
    }
    sys_ready = ready->sysevt_ready_rsp == WIRELESS_FW_RUNNING;
    if (!sys_ready) {
      log_printf("BLE: CPU2 runs the FUS, not the wireless stack");
      dead = true;
    }
  } else if (evt->subevtcode == SHCI_SUB_EVT_ERROR_NOTIF) {
    log_printf("BLE: CPU2 error 0x%02X", evt->payload[0]);
  }
}

/* ------------------------------------------------------- file service */

static tBleStatus notify(uint16_t chr, const void *data, size_t len)
{
  /* to every link that enabled the notifications: the file service's are enabled by the configurator only */
  return aci_gatt_update_char_value(fs_svc, chr, 0, (uint8_t)len, (const uint8_t *)data);
}

static size_t payload_max(void)
{
  const link_t *l = link_by(fs_link);
  uint16_t mtu = l ? l->mtu : info.mtu;
  size_t m = mtu > 3U ? mtu - 3U : 20U;
  return m > VALUE_MAX ? VALUE_MAX : m;
}

static void status_send(const char *fmt, ...)
{
  char line[VALUE_MAX + 2];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line) - 1U, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  size_t len = (size_t)n < sizeof(line) - 2U ? (size_t)n : sizeof(line) - 2U;
  line[len++] = '\n';
  /* A line longer than one notification (small MTU) goes out in pieces; the client joins them up to '\n'. */
  for (size_t off = 0; off < len;) {
    size_t part = len - off < payload_max() ? len - off : payload_max();
    uint32_t t0 = HAL_GetTick();
    tBleStatus ret;
    while ((ret = notify(fs_status, line + off, part)) == BLE_STATUS_INSUFFICIENT_RESOURCES &&
           HAL_GetTick() - t0 < 200U) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (ret != BLE_STATUS_SUCCESS) {
      return;
    }
    off += part;
  }
}

#define CUSTOM_REL "voices/custom"

static bool plain_name(const char *s, size_t len, size_t max)
{
  if (len == 0 || len > max) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}

/* "voices/custom/<name>.qoa" or "voices/custom/<folder>/<name>.qoa" with plain names: a user-made
 * voice clip (docs/ble-file-service.md); the folder is the clip's folder in the pack (al, sp, nav, time). */
static bool clip_file(const char *name)
{
  static const char prefix[] = CUSTOM_REL "/";
  if (strncmp(name, prefix, sizeof(prefix) - 1U) != 0) {
    return false;
  }
  const char *n = name + sizeof(prefix) - 1U;
  const char *slash = strchr(n, '/');
  if (slash) {
    if (!plain_name(n, (size_t)(slash - n), 8)) {
      return false;
    }
    n = slash + 1;
  }
  const char *dot = strstr(n, ".qoa");
  if (!dot || dot[4] != 0) {
    return false;
  }
  return plain_name(n, (size_t)(dot - n), 24);
}

/* Where a file of the service lives on the card: our extension file in its folder, the rest in the top folder. */
static void file_path(char *out, size_t size, const char *name)
{
  if (strcmp(name, "config_ext.ultrabip") == 0) {
    storage_mkdir(CONFIG_EXT_DIR);
    snprintf(out, size, "%s", CONFIG_EXT_PATH);
  } else if (strcmp(name, "if_found.txt") == 0) {
    storage_mkdir(CONFIG_EXT_DIR);
    snprintf(out, size, "%s", IF_FOUND_MASTER_PATH);
  } else {
    snprintf(out, size, "0:/%s", name);
  }
}

static bool allowed_file(const char *name)
{
  return strcmp(name, "config.ultrabip") == 0 || strcmp(name, "config_ext.ultrabip") == 0 ||
         strcmp(name, "if_found.txt") == 0 || clip_file(name);
}

static const char *const track_dirs[] = {"tracks_IGC", "tracks_KML"};

static bool is_track_dir(const char *dir)
{
  return strcmp(dir, track_dirs[0]) == 0 || strcmp(dir, track_dirs[1]) == 0;
}

static bool is_year(const char *s)
{
  return strlen(s) == 4 && s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9' &&
         s[3] >= '0' && s[3] <= '9';
}

/* "tracks_IGC/<file>" or "tracks_IGC/<year>/<file>" (and the same for KML), no other sub folders, no "..". */
static bool track_file(const char *name, char *path, size_t size)
{
  for (unsigned i = 0; i < 2; i++) {
    size_t n = strlen(track_dirs[i]);
    if (strncmp(name, track_dirs[i], n) == 0 && name[n] == '/') {
      const char *file = name + n + 1;
      const char *slash = strchr(file, '/');
      if (slash && slash - file == 4) {
        char year[5];
        memcpy(year, file, 4);
        year[4] = 0;
        file = is_year(year) ? slash + 1 : file;
      }
      if (!file[0] || strchr(file, '/') || strchr(file, '\\') || strstr(file, "..")) {
        return false;
      }
      snprintf(path, size, "0:/%s", name);
      return true;
    }
  }
  return false;
}

static void xf_reset(void)
{
  if (xf == XF_PUT_FILE) {
    put_file_abort();
  }
  if (xf_file_open) {
    storage_lock();
    f_close(&xf_file);
    storage_unlock();
    xf_file_open = false;
  }
  xf = XF_IDLE;
  xf_ack = false;
  put_buffered = 0;
}

/* "<prefix><name> <size>\n" per file of a folder into xfer_buf; false if the listing is full. */
static bool list_files(const char *path, const char *prefix)
{
  DIR d;
  FILINFO fi;
  if (f_opendir(&d, path) != FR_OK) {
    return true;
  }
  bool room = true;
  while (room && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
    if (fi.fattrib & AM_DIR) {
      continue;
    }
    int n = snprintf(xfer_buf + xf_size, XFER_MAX - xf_size, "%s%s %lu\n", prefix, fi.fname, (unsigned long)fi.fsize);
    if (n < 0 || (uint32_t)n >= XFER_MAX - xf_size) {
      room = false; /* listing full */
    } else {
      xf_size += (uint32_t)n;
    }
  }
  f_closedir(&d);
  return room;
}

/* The files of a track folder and of its year folders ("2026/<name> <size>"), or of voices/custom and its
 * clip folders ("al/<name> <size>"); false if it cannot be read. */
static bool list_dir(const char *dir)
{
  char path[72], sub[96], prefix[16];
  DIR d;
  FILINFO fi;
  bool clips = strcmp(dir, CUSTOM_REL) == 0;
  snprintf(path, sizeof(path), "0:/%s", dir);
  xf_size = 0;
  storage_lock();
  bool ok = f_opendir(&d, path) == FR_OK;
  bool room = ok && list_files(path, "");
  while (room && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
    if ((fi.fattrib & AM_DIR) && (clips ? strlen(fi.fname) <= 8 : is_year(fi.fname))) {
      snprintf(sub, sizeof(sub), "%s/%.8s", path, fi.fname);
      snprintf(prefix, sizeof(prefix), "%.8s/", fi.fname);
      room = list_files(sub, prefix);
    }
  }
  if (ok) {
    f_closedir(&d);
  }
  storage_unlock();
  return ok;
}

static void copy_word(char *dst, size_t size, const char *s)
{
  size_t i = 0;
  for (; s[i] && i < size - 1U; i++) {
    dst[i] = s[i] == ' ' ? '_' : s[i];
  }
  dst[i] = 0;
}

/* "LIVE" reply: sensors, GPS, battery and state in one line (configurator status view). */
static void send_live(void)
{
  sensors_t s;
  gps_fix_t fx;
  recorder_status_t rec;
  char st[24];
  sensors_get(&s);
  gps_get_fix(&fx);
  recorder_get_status(&rec);
  copy_word(st, sizeof(st), system_state_name());
  gps_status_t g;
  gps_get(&g);
  /* hardware: 1 ok, 0 failed, - not running */
  status_send("LIVE st=%s p=%.1f t=%.1f alt=%.1f vz=%.2f acc=%.2f rest=%d fused=%d fix=%d sats=%u hdop=%.1f "
              "gs=%.1f lat=%.6f lon=%.6f vbat=%lu rec=%d piezo=%s quiet=%lu pump=%lu taps=%lu tapg=%.2f tsens=%d "
              "baro=%s imu=%s gpsmod=%s gpsr=%lu sd=%d pack=%d",
              st, (double)s.pressure_pa, (double)s.temp_c, (double)s.alt_m, (double)s.vario_ms, (double)s.accel_ms2,
              s.at_rest, s.fused, fx.valid && fx.fix_type == 3, fx.sats, (double)fx.hdop, (double)(fx.speed_ms * 3.6f),
              fx.lat_e7 / 1e7, fx.lon_e7 / 1e7, (unsigned long)system_battery_mv(), rec.active,
              piezo_get_drive() == PIEZO_DRIVE_PWM ? "pwm" : "classic", (unsigned long)sound_quiet_pulse(),
              (unsigned long)sound_loud_pump(), (unsigned long)system_double_taps(), (double)sensors_last_tap_g(),
              sensors_tap_sensitivity(), !s.on ? "-" : s.baro_ok ? "1" : "0", !s.on ? "-" : s.imu_ok ? "1" : "0",
              g.state == GPS_OFF ? "-" : g.alive ? "1" : "0", (unsigned long)g.restarts, storage_is_mounted(),
              storage_voice_pack_ready());
}

/* Tests from the configurator: tones and the vario on the device, only on the ground. */
static bool handle_test_command(const char *cmd)
{
  unsigned f, ms, vol;
  char clip[32];
  int climb_cms;
  bool is_test = strncmp(cmd, "TONE ", 5) == 0 || strncmp(cmd, "SPK ", 4) == 0 || strncmp(cmd, "SAY ", 4) == 0 ||
                 strncmp(cmd, "VARIO ", 6) == 0 || strncmp(cmd, "SIM ", 4) == 0 || strncmp(cmd, "TONES ", 6) == 0 ||
                 strncmp(cmd, "PIEZO ", 6) == 0 || strncmp(cmd, "TAPSENS ", 8) == 0 || strcmp(cmd, "TEST off") == 0 ||
                 strncmp(cmd, "MELODY ", 7) == 0 || strncmp(cmd, "ANNOUNCE ", 9) == 0;
  if (!is_test) {
    return false;
  }
  if (strcmp(cmd, "TEST off") == 0) {
    system_test_stop();
    status_send("OK");
    return true;
  }
  if (!system_test_allowed()) {
    status_send("ERR %s", system_in_flight() ? "in flight" : "not switched on");
    return true;
  }
  vario_tones_t t = config_get()->profile[system_profile() - 1U].tones; /* flags TONES leaves out stay the profile's */
  if (sscanf(cmd, "TONE %u %u %u", &f, &ms, &vol) == 3) {
    sound_tone((uint16_t)(f > 5000 ? 5000 : f), (uint16_t)(ms > 3000 ? 3000 : ms), (uint8_t)(vol > 100 ? 100 : vol), false);
  } else if (sscanf(cmd, "SPK %u %u %u", &f, &ms, &vol) == 3) {
    sound_tone((uint16_t)(f > 8000 ? 8000 : f), (uint16_t)(ms > 3000 ? 3000 : ms), (uint8_t)(vol > 100 ? 100 : vol), true);
  } else if (sscanf(cmd, "SAY %31s %u", clip, &vol) == 2) {
    sound_say1(clip, (uint8_t)(vol > 100 ? 100 : vol));
  } else if (sscanf(cmd, "VARIO %u", &vol) == 1) {
    system_vario_test((uint8_t)(vol > 100 ? 100 : vol));
  } else if (strcmp(cmd, "SIM off") == 0) {
    vario_audio_simulate(false, 0.0f);
  } else if (sscanf(cmd, "SIM %d", &climb_cms) == 1) { /* cm/s: no float scanf in newlib nano */
    climb_cms = climb_cms < -1000 ? -1000 : climb_cms > 1000 ? 1000 : climb_cms;
    vario_audio_simulate(true, (float)climb_cms / 100.0f);
  } else if (strncmp(cmd, "TONES ", 6) == 0 && vario_tones_parse(cmd + 6, &t)) {
    system_test_tones(&t);
  } else if (strncmp(cmd, "ANNOUNCE ", 9) == 0) {
    char trigger[16], items[160];
    if (sscanf(cmd + 9, "%15s %159s", trigger, items) < 1) {
      status_send("ERR arguments");
      return true;
    }
    system_test_announce(trigger, items);
  } else if (sscanf(cmd, "MELODY %31s %u", clip, &vol) == 2) {
    unsigned i = 0;
    while (i < SOUND_POWER_OFF_COUNT && strcmp(clip, sound_power_off_names[i]) != 0) {
      i++;
    }
    if (i >= SOUND_POWER_OFF_COUNT) {
      status_send("ERR melody");
      return true;
    }
    sound_play_power_off(i, (uint8_t)(vol > 100 ? 100 : vol));
  } else if (strcmp(cmd, "PIEZO classic") == 0 || strcmp(cmd, "PIEZO pwm") == 0) {
    /* Experiment: stays until the device is switched off; TEST off keeps it. */
    piezo_set_drive(cmd[6] == 'p' ? PIEZO_DRIVE_PWM : PIEZO_DRIVE_CLASSIC);
    log_printf("Piezo drive: %s", cmd + 6);
  } else if (sscanf(cmd, "PIEZO pulse %u", &f) == 1) {
    sound_set_quiet_pulse(f); /* step "very quiet", until switched off */
  } else if (sscanf(cmd, "PIEZO pump %u", &f) == 1) {
    sound_set_loud_pump(f); /* step "loud", until switched off */
  } else if (sscanf(cmd, "TAPSENS %u", &f) == 1) {
    sensors_set_tap_sensitivity((int)f); /* until switched off or the next profile choice */
  } else {
    status_send("ERR arguments");
    return true;
  }
  status_send("OK");
  return true;
}

/*
 * Accelerometer calibration of the configurator (docs/ble-file-service.md):
 *   CAL                         mean of the raw accelerometer over the last second, still flag,
 *                               the calibration in use
 *   CAL set ox oy oz sx sy sz   offsets [mm/s²] and scales [ppm of 1]: applied at once and saved
 *   CAL reset                   no calibration (saved)
 */
static bool handle_cal_command(const char *cmd)
{
  if (strncmp(cmd, "CAL", 3) != 0 || (cmd[3] != 0 && cmd[3] != ' ')) {
    return false;
  }
  if (!system_test_allowed()) {
    status_send("ERR %s", system_in_flight() ? "in flight" : "not switched on");
    return true;
  }
  float off[3], scale[3];
  long v[6];
  if (strcmp(cmd, "CAL") == 0) {
    sensors_cal_sample_t c;
    sensors_cal_sample(&c);
    sensors_accel_calibration(off, scale);
    status_send("CAL n=%lu still=%d ax=%.4f ay=%.4f az=%.4f ox=%.4f oy=%.4f oz=%.4f sx=%.6f sy=%.6f sz=%.6f",
                (unsigned long)c.seq, c.still ? 1 : 0, (double)c.mean[0], (double)c.mean[1], (double)c.mean[2],
                (double)off[0], (double)off[1], (double)off[2], (double)scale[0], (double)scale[1],
                (double)scale[2]);
    return true;
  }
  if (strcmp(cmd, "CAL reset") == 0) {
    for (int k = 0; k < 3; k++) {
      off[k] = 0.0f;
      scale[k] = 1.0f;
    }
  } else if (sscanf(cmd, "CAL set %ld %ld %ld %ld %ld %ld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
    for (int k = 0; k < 3; k++) {
      if (v[k] < -3000 || v[k] > 3000 || v[3 + k] < 800000L || v[3 + k] > 1200000L) {
        status_send("ERR out of range");
        return true;
      }
      off[k] = (float)v[k] / 1000.0f;
      scale[k] = (float)v[3 + k] / 1e6f;
    }
  } else {
    status_send("ERR arguments");
    return true;
  }
  sensors_set_accel_calibration(off, scale);
  bool saved = config_save_accel_calibration(off, scale);
  log_printf("Accelerometer calibration: offset %.3f %.3f %.3f m/s2, scale %.5f %.5f %.5f%s", (double)off[0],
             (double)off[1], (double)off[2], (double)scale[0], (double)scale[1], (double)scale[2],
             saved ? "" : " (NOT saved)");
  status_send(saved ? "OK" : "ERR not saved");
  return true;
}

/*
 * "AUTH <secret>": the configurator's authorisation (docs/ble-file-service.md). A secret the pilot confirmed
 * before is accepted at once ("OK"); a new one waits for a short button press ("PENDING", then "AUTH ok" or
 * "AUTH denied" after the 60 s window). One configurator at a time: a second link gets "ERR busy".
 */
static void handle_auth(link_t *l, const char *secret)
{
  size_t n = strlen(secret);
  if (n == 0 || n > BIPLINK_SECRET_MAX || !plain_name(secret, n, BIPLINK_SECRET_MAX)) {
    status_send("ERR secret");
    return;
  }
  if (l->fs_authorized) {
    status_send("OK");
    return;
  }
  if (fs_link != NO_LINK && fs_link != l->handle && link_by(fs_link)) {
    status_send("ERR busy"); /* another configurator is connected */
    return;
  }
  memcpy(l->fs_secret, secret, n); /* n <= BIPLINK_SECRET_MAX, checked above */
  l->fs_secret[n] = 0;
  biplink_auth_request(secret, false);
  if (biplink_secret_accepted(secret)) {
    l->fs_authorized = true;
    l->fs_auth_wait = false;
    fs_link = l->handle;
    status_send("OK");
    log_printf("BLE: configurator authorised");
    return;
  }
  l->fs_auth_wait = true;
  status_send("PENDING");
}

/* Once per BLE task loop: an AUTH that waited for the button gets its answer. */
static void auth_tick(void)
{
  for (unsigned i = 0; i < LINKS_MAX; i++) {
    link_t *l = &links[i];
    if (l->handle == NO_LINK || !l->fs_auth_wait) {
      continue;
    }
    if (biplink_secret_accepted(l->fs_secret)) {
      l->fs_auth_wait = false;
      l->fs_authorized = true;
      fs_link = l->handle;
      status_send("AUTH ok");
      log_printf("BLE: configurator authorised by the button");
    } else if (!biplink_auth_pending()) {
      l->fs_auth_wait = false;
      status_send("AUTH denied");
    }
  }
}

static void handle_command(uint16_t handle, const uint8_t *data, uint16_t len)
{
  char cmd[VALUE_MAX + 1];
  char name[64];
  char opt[8];
  unsigned long size = 0;
  unsigned long crc = 0;
  int args;
  size_t n = len < sizeof(cmd) - 1U ? len : sizeof(cmd) - 1U;
  memcpy(cmd, data, n);
  cmd[n] = 0;
  while (n && (cmd[n - 1] == '\r' || cmd[n - 1] == '\n' || cmd[n - 1] == ' ')) {
    cmd[--n] = 0;
  }

  link_t *l = link_by(handle);
  if (!l) {
    return;
  }
  if (strncmp(cmd, "AUTH ", 5) == 0) {
    handle_auth(l, cmd + 5);
    return;
  }
  if (!l->fs_authorized && strcmp(cmd, "INFO") != 0) {
    status_send("ERR not authorised");
    return;
  }
  if (l->fs_authorized) {
    fs_link = handle;
  }
  if (handle_test_command(cmd) || handle_cal_command(cmd)) {
    return;
  }
  if (strncmp(cmd, "CON ", 4) == 0 || strcmp(cmd, "CON") == 0) {
    /* a console line: no reply, its echo and answer come as console output */
    (void)ble_console_input(cmd[3] ? cmd + 4 : "");
  } else if (strcmp(cmd, "CONSOLE on") == 0 || strcmp(cmd, "CONSOLE off") == 0) {
    ble_console_enable(strcmp(cmd + 8, "on") == 0);
    status_send("OK");
  } else if (strcmp(cmd, "LIVE") == 0) {
    send_live();
  } else if (strcmp(cmd, "INFO") == 0) {
    char state[24];
    copy_word(state, sizeof(state), system_state_name());
    status_send("INFO name=UltraBeep version=%s profile=%u state=%s mtu=%u interval=%u.%02u phy=%u links=%u auth=%d",
                FW_VERSION, system_profile(), state, l->mtu, l->interval * 125U / 100U, l->interval * 125U % 100U,
                l->phy, (unsigned)link_count(), l->fs_authorized ? 1 : 0);
  } else if (sscanf(cmd, "LIST %63s", name) == 1) {
    if (xf != XF_IDLE) {
      status_send("ERR busy");
    } else if (!is_track_dir(name) && strcmp(name, CUSTOM_REL) != 0) {
      status_send("ERR not allowed");
    } else {
      if (!list_dir(name)) {
        xf_size = 0; /* no such folder yet: empty listing */
      }
      xf_pos = 0;
      xf_crc = crc32(xfer_buf, xf_size);
      xf = XF_GET;
      status_send("OK %lu", (unsigned long)xf_size);
    }
  } else if (sscanf(cmd, "GET %63s", name) == 1 && track_file(name, xf_path, sizeof(xf_path))) {
    if (xf != XF_IDLE) {
      status_send("ERR busy");
    } else {
      storage_lock();
      xf_file_open = f_open(&xf_file, xf_path, FA_READ) == FR_OK;
      xf_size = xf_file_open ? (uint32_t)f_size(&xf_file) : 0U;
      storage_unlock();
      if (!xf_file_open) {
        status_send("ERR not found");
      } else {
        xf_pos = 0;
        xf_buf_len = xf_buf_pos = 0;
        xf_crc = 0;
        xf = XF_STREAM;
        status_send("OK %lu", (unsigned long)xf_size);
        log_printf("BLE: sending %s (%lu bytes)", xf_path, (unsigned long)xf_size);
      }
    }
  } else if (sscanf(cmd, "GET %63s", name) == 1) {
    if (xf != XF_IDLE) {
      status_send("ERR busy");
    } else if (!allowed_file(name)) {
      status_send("ERR not allowed");
    } else {
      file_path(xf_path, sizeof(xf_path), name);
      int got = storage_read_text(xf_path, xfer_buf, sizeof(xfer_buf));
      if (got < 0) {
        status_send("ERR not found");
      } else {
        xf_size = (uint32_t)got;
        xf_pos = 0;
        xf_crc = crc32(xfer_buf, xf_size);
        xf = XF_GET;
        status_send("OK %lu", (unsigned long)xf_size);
        log_printf("BLE: sending %s (%lu bytes)", name, (unsigned long)xf_size);
      }
    }
  } else if (sscanf(cmd, "DEL %63s", name) == 1) {
    if (!clip_file(name)) {
      status_send("ERR not allowed");
    } else {
      char path[80];
      snprintf(path, sizeof(path), "0:/%s", name);
      bool ok = storage_remove(path);
      log_printf("BLE: delete %s: %s", name, ok ? "ok" : "not found");
      status_send(ok ? "OK" : "ERR not found");
    }
  } else if (strcmp(cmd, "UPDATE") == 0) {
    if (system_in_flight()) {
      status_send("ERR in flight");
    } else if (system_battery_mv() && system_battery_mv() < UPDATE_MIN_MV) {
      status_send("ERR battery");
    } else {
      status_send("OK");
      log_printf("BLE: update requested");
      system_request_update();
    }
  } else if ((args = sscanf(cmd, "PUT %63s %lu %lx %7s", name, &size, &crc, opt)) >= 3) {
    bool ack = args == 4 && strcmp(opt, "ack") == 0;
    if (xf != XF_IDLE) {
      status_send("ERR busy");
    } else if (strcmp(name, "update") == 0) {
      if (size == 0 || size > UPDATE_MAX) {
        status_send("ERR size");
      } else if (system_in_flight()) {
        status_send("ERR in flight");
      } else if (!storage_write_open(UPDATE_TMP_PATH)) {
        status_send("ERR write");
      } else {
        xf_size = (uint32_t)size;
        xf_crc = (uint32_t)crc;
        xf_run_crc = 0;
        xf_pos = 0;
        put_buffered = 0;
        xf_tick = HAL_GetTick();
        xf = XF_PUT_FILE;
        put_ready(ack);
      }
    } else if (!allowed_file(name)) {
      status_send("ERR not allowed");
    } else if (size == 0 || size > XFER_MAX) {
      status_send("ERR size");
    } else {
      file_path(xf_path, sizeof(xf_path), name);
      xf_size = (uint32_t)size;
      xf_crc = (uint32_t)crc;
      xf_pos = 0;
      xf_tick = HAL_GetTick();
      xf = XF_PUT;
      put_ready(ack);
    }
  } else {
    status_send("ERR unknown command");
  }
}

static void put_ready(bool ack)
{
  xf_ack = ack;
  xf_start = HAL_GetTick();
  if (ack) {
    status_send("READY ack=%u window=%u", ACK_STEP, PUT_WINDOW);
  } else {
    status_send("READY");
  }
}

/* After len more bytes arrived: an ACK when a step boundary was crossed (DONE follows the last bytes anyway). */
static void put_ack(uint16_t len)
{
  if (xf_ack && xf_pos < xf_size && xf_pos / ACK_STEP != (xf_pos - len) / ACK_STEP) {
    status_send("ACK %lu", (unsigned long)xf_pos);
  }
}

/* Drops a streamed image that did not arrive whole. */
static void put_file_abort(void)
{
  storage_write_close();
  storage_remove(UPDATE_TMP_PATH);
  xf = XF_IDLE;
  xf_ack = false;
  put_buffered = 0;
}

static void handle_put_file(const uint8_t *data, uint16_t len)
{
  if (xf_pos + len > xf_size) {
    put_file_abort();
    status_send("ERR size");
    return;
  }
  memcpy(xfer_buf + put_buffered, data, len);
  put_buffered += len;
  xf_run_crc = crc32_update(xf_run_crc, data, len);
  xf_pos += len;
  xf_tick = HAL_GetTick();
  /* whole pieces of FILE_FLUSH bytes, and whatever is left with the last bytes */
  while (put_buffered >= FILE_FLUSH || (put_buffered && xf_pos >= xf_size)) {
    uint32_t n = put_buffered < FILE_FLUSH ? put_buffered : FILE_FLUSH;
    if (!storage_write_chunk(xfer_buf, n)) {
      put_file_abort();
      status_send("ERR write");
      return;
    }
    put_buffered -= n;
    memmove(xfer_buf, xfer_buf + n, put_buffered);
  }
  put_ack(len);
  if (xf_pos < xf_size) {
    return;
  }
  xf = XF_IDLE;
  xf_ack = false;
  bool ok = storage_write_close();
  if (!ok || xf_run_crc != xf_crc) {
    storage_remove(UPDATE_TMP_PATH);
    status_send(ok ? "ERR crc" : "ERR write");
    return;
  }
  ok = storage_rename(UPDATE_TMP_PATH, UPDATE_PATH);
  log_printf("BLE: received firmware image (%lu bytes in %lu ms) %s", (unsigned long)xf_size,
             (unsigned long)(HAL_GetTick() - xf_start), ok ? "saved" : "NOT saved");
  if (ok) {
    status_send("DONE %08lX", (unsigned long)xf_run_crc);
  } else {
    status_send("ERR write");
  }
}

static void handle_put_data(uint16_t handle, const uint8_t *data, uint16_t len)
{
  if (handle != fs_link) {
    return; /* only the configurator's link feeds a transfer */
  }
  if (xf == XF_PUT_FILE) {
    handle_put_file(data, len);
    return;
  }
  if (xf != XF_PUT) {
    return;
  }
  if (xf_pos + len > xf_size) {
    xf = XF_IDLE;
    status_send("ERR size");
    return;
  }
  memcpy(xfer_buf + xf_pos, data, len);
  xf_pos += len;
  xf_tick = HAL_GetTick();
  put_ack(len);
  if (xf_pos < xf_size) {
    return;
  }
  xf = XF_IDLE;
  xf_ack = false;
  uint32_t crc = crc32(xfer_buf, xf_size);
  if (crc != xf_crc) {
    status_send("ERR crc");
    return;
  }
  if (strncmp(xf_path, "0:/" CUSTOM_REL "/", 17) == 0) {
    storage_mkdir("0:/voices");
    storage_mkdir(CUSTOM_DIR);
    char dir[80];
    snprintf(dir, sizeof(dir), "%s", xf_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
      *slash = 0;
      if (strlen(dir) > strlen(CUSTOM_DIR)) {
        storage_mkdir(dir); /* the clip's folder (al, sp, nav, time) */
      }
    }
  }
  bool ok = storage_write_text(xf_path, xfer_buf, xf_size);
  log_printf("BLE: received %s (%lu bytes in %lu ms) %s", xf_path, (unsigned long)xf_size,
             (unsigned long)(HAL_GetTick() - xf_start), ok ? "saved" : "NOT saved");
  if (ok && strcmp(xf_path, IF_FOUND_MASTER_PATH) == 0) {
    config_request_if_found_mirror(); /* the finder's copy in the top folder follows (system task) */
  }
  if (ok) {
    status_send("DONE %08lX", (unsigned long)crc);
  } else {
    status_send("ERR write");
  }
}

static void transfer_tick(void)
{
  if ((xf == XF_PUT || xf == XF_PUT_FILE) && HAL_GetTick() - xf_tick > PUT_TIMEOUT_MS) {
    if (xf == XF_PUT_FILE) {
      put_file_abort();
    }
    xf = XF_IDLE;
    status_send("ERR timeout");
  }
  if (xf != XF_GET && xf != XF_STREAM) {
    return;
  }
  if (!tx_pool_free) {
    if (HAL_GetTick() - tx_pool_full_tick < 500U) {
      return;
    }
    tx_pool_free = true; /* missed the pool event: retry */
  }
  for (int burst = 0; burst < 32 && xf_pos < xf_size; burst++) {
    const char *src;
    size_t avail;
    if (xf == XF_STREAM) {
      if (xf_buf_pos >= xf_buf_len) {
        /* next chunk from the card; the CRC runs over what is sent */
        uint32_t want = xf_size - xf_pos < STREAM_CHUNK ? xf_size - xf_pos : STREAM_CHUNK;
        UINT br = 0;
        storage_lock();
        FRESULT fr = f_read(&xf_file, xfer_buf, want, &br);
        storage_unlock();
        if (fr != FR_OK || br == 0) {
          xf_reset();
          status_send("ERR read");
          return;
        }
        xf_crc = crc32_update(xf_crc, xfer_buf, br);
        xf_buf_len = br;
        xf_buf_pos = 0;
      }
      src = xfer_buf + xf_buf_pos;
      avail = xf_buf_len - xf_buf_pos;
    } else {
      src = xfer_buf + xf_pos;
      avail = xf_size - xf_pos;
    }
    size_t n = payload_max();
    if (n > avail) {
      n = avail;
    }
    tBleStatus ret = notify(fs_data, src, n);
    if (ret == BLE_STATUS_INSUFFICIENT_RESOURCES) {
      tx_pool_free = false;
      tx_pool_full_tick = HAL_GetTick();
      return;
    }
    if (ret != BLE_STATUS_SUCCESS) {
      xf_reset();
      status_send("ERR send 0x%02X", ret);
      return;
    }
    xf_pos += n;
    xf_buf_pos += n;
  }
  if (xf_pos >= xf_size) {
    xf_reset();
    status_send("DONE %08lX", (unsigned long)xf_crc);
  }
}

/* ------------------------------------------------------------ HCI events */

static void start_advertising(bool slow)
{
  uint8_t local_name[sizeof(dev_name) + 1];
  size_t n = strlen(dev_name);
  local_name[0] = AD_TYPE_COMPLETE_LOCAL_NAME;
  memcpy(local_name + 1, dev_name, n);
  uint16_t min = slow ? 0x500 : 0xA0; /* units of 0.625 ms */
  uint16_t max = slow ? 0x640 : 0x140;
  tBleStatus ret = aci_gap_set_discoverable(ADV_IND, min, max, GAP_PUBLIC_ADDR, 0x00, (uint8_t)(n + 1),
                                            local_name, 0, NULL, 0, 0);
  advertising = ret == BLE_STATUS_SUCCESS;
  adv_slow = slow;
  adv_tick = HAL_GetTick();
  if (!advertising) {
    adv_fail_tick = HAL_GetTick();
    if (!adv_fail_logged) {
      adv_fail_logged = true;
      log_printf("BLE: advertising failed 0x%02X%s", ret, link_count() ? " (while connected)" : "");
    }
  } else {
    adv_fail_logged = false;
  }
}

static void stop_advertising(void)
{
  if (advertising) {
    aci_gap_set_non_discoverable();
    advertising = false;
  }
}

/*
 * Advertising follows the links: always without one (fast for ADV_FAST_MS, then slow), with one link
 * only on the ground and at the slow interval (so the configurator finds the device next to the phone),
 * never with two. The stack stops advertising by itself when a central connects.
 */
static void advertising_reconcile(void)
{
  unsigned n = link_count();
  bool want = n == 0 || (n < LINKS_MAX && system_second_link_allowed());
  if (!want) {
    stop_advertising();
  } else if (!advertising) {
    if (HAL_GetTick() - adv_fail_tick >= ADV_RETRY_MS || adv_fail_tick == 0) {
      start_advertising(n > 0);
    }
  } else if (n == 0 && !adv_slow && HAL_GetTick() - adv_tick > ADV_FAST_MS) {
    stop_advertising();
    start_advertising(true);
  } else if (n > 0 && !adv_slow) {
    stop_advertising();
    start_advertising(true);
  }
  info.running = host_ready && (advertising || n > 0);
}

static void on_connected(uint16_t handle, uint8_t status, const uint8_t *peer, uint16_t interval)
{
  if (status != 0) {
    return;
  }
  advertising = false; /* the stack stops it at a connection */
  link_t *l = link_free();
  if (!l) {
    log_printf("BLE: no room for a third link, disconnecting");
    hci_disconnect(handle, 0x13);
    return;
  }
  memset(l, 0, sizeof(*l));
  l->handle = handle;
  l->mtu = 23;
  l->interval = interval;
  l->phy = 1;
  l->tick = HAL_GetTick();
  info_refresh();
  biplink_on_connect();
  if (info.links == 1) {
    remote_on_connect();
  }
  log_printf("BLE: connected to %02X:%02X:%02X:%02X:%02X:%02X, interval %u.%02u ms", peer[5], peer[4], peer[3],
             peer[2], peer[1], peer[0], interval * 125U / 100U, interval * 125U % 100U);
  /* Big packets and the 2 Mbit/s PHY for the file transfers; the central decides. */
  aci_gatt_exchange_config(handle);
  hci_le_set_data_length(handle, 251, 2120);
  hci_le_set_phy(handle, 0, 0x02, 0x02, 0);
}

static void on_vendor_event(uint16_t code, const uint8_t *data)
{
  if (code == ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) {
    const aci_gatt_attribute_modified_event_rp0 *e = (const aci_gatt_attribute_modified_event_rp0 *)data;
    if (biplink_on_attribute_modified(e->Attr_Handle, e->Attr_Data, e->Attr_Data_Length)) {
      bl_link = e->Connection_Handle; /* the app's link */
    } else if (remote_on_attribute_modified(e->Attr_Handle, e->Attr_Data, e->Attr_Data_Length)) {
      remote_link = e->Connection_Handle;
    } else if (e->Attr_Handle == fs_cmd + 1) {
      handle_command(e->Connection_Handle, e->Attr_Data, e->Attr_Data_Length);
    } else if (e->Attr_Handle == fs_data + 1) {
      handle_put_data(e->Connection_Handle, e->Attr_Data, e->Attr_Data_Length);
    }
  } else if (code == ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE) {
    const aci_att_exchange_mtu_resp_event_rp0 *e = (const aci_att_exchange_mtu_resp_event_rp0 *)data;
    link_t *l = link_by(e->Connection_Handle);
    if (l) {
      l->mtu = e->Server_RX_MTU;
      info_refresh();
    }
  } else if (code == ACI_GATT_TX_POOL_AVAILABLE_VSEVT_CODE) {
    tx_pool_free = true;
  } else if (code == ACI_GAP_BOND_LOST_VSEVT_CODE) {
    /* the phone forgot the bond: pair anew (the remote's link, else the newest) */
    const link_t *l = link_by(remote_link);
    for (unsigned i = 0; !l && i < LINKS_MAX; i++) {
      if (links[i].handle != NO_LINK) {
        l = &links[i];
      }
    }
    if (l) {
      aci_gap_allow_rebond(l->handle);
    }
    log_printf("BLE: bond lost, pairing again");
  } else if (code == ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE) {
    const aci_gap_pairing_complete_event_rp0 *e = (const aci_gap_pairing_complete_event_rp0 *)data;
    log_printf("BLE: pairing %s (status 0x%02X, reason 0x%02X)", e->Status == 0 ? "done" : "failed", e->Status,
               e->Reason);
  }
}

static void hci_user_evt(void *payload)
{
  tHCI_UserEvtRxParam *param = (tHCI_UserEvtRxParam *)payload;
  TL_Evt_t *evt = &param->pckt->evtserial.evt;
  if (evt->evtcode == HCI_DISCONNECTION_COMPLETE_EVT_CODE) {
    const hci_disconnection_complete_event_rp0 *e = (const hci_disconnection_complete_event_rp0 *)evt->payload;
    uint16_t h = e->Connection_Handle;
    link_t *l = link_by(h);
    if (l) {
      l->handle = NO_LINK;
    }
    unsigned left = link_count();
    log_printf("BLE: disconnected (reason 0x%02X), %u link%s left", e->Reason, left, left == 1 ? "" : "s");
    /* each session ends with its link; with none left everything is reset (a handle we never saw included) */
    if (h == fs_link || !left) {
      fs_link = NO_LINK;
      xf_reset();
      ble_console_enable(false);
      system_test_stop(); /* tests from the configurator end with the connection */
    }
    if (h == bl_link || !left) {
      bl_link = NO_LINK;
      biplink_on_disconnect();
    }
    if (h == remote_link || !left) {
      remote_link = NO_LINK;
      remote_on_disconnect();
    }
    info_refresh();
    adv_fail_tick = 0; /* advertise again at once */
  } else if (evt->evtcode == HCI_LE_META_EVT_CODE) {
    uint8_t sub = evt->payload[0];
    const uint8_t *data = &evt->payload[1];
    if (sub == HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE) {
      const hci_le_connection_complete_event_rp0 *e = (const hci_le_connection_complete_event_rp0 *)data;
      on_connected(e->Connection_Handle, e->Status, e->Peer_Address, e->Conn_Interval);
    } else if (sub == HCI_LE_ENHANCED_CONNECTION_COMPLETE_SUBEVT_CODE) {
      const hci_le_enhanced_connection_complete_event_rp0 *e =
        (const hci_le_enhanced_connection_complete_event_rp0 *)data;
      on_connected(e->Connection_Handle, e->Status, e->Peer_Address, e->Conn_Interval);
    } else if (sub == HCI_LE_CONNECTION_UPDATE_COMPLETE_SUBEVT_CODE) {
      const hci_le_connection_update_complete_event_rp0 *e =
        (const hci_le_connection_update_complete_event_rp0 *)data;
      link_t *l = link_by(e->Connection_Handle);
      if (e->Status == 0 && l) {
        l->interval = e->Conn_Interval;
        info_refresh();
      }
      log_printf("BLE: connection interval %u.%02u ms (status 0x%02X)", e->Conn_Interval * 125U / 100U,
                 e->Conn_Interval * 125U % 100U, e->Status);
    } else if (sub == HCI_LE_PHY_UPDATE_COMPLETE_SUBEVT_CODE) {
      const hci_le_phy_update_complete_event_rp0 *e = (const hci_le_phy_update_complete_event_rp0 *)data;
      link_t *l = link_by(e->Connection_Handle);
      if (e->Status == 0 && l) {
        l->phy = e->TX_PHY;
        info_refresh();
      }
      log_printf("BLE: PHY %u Mbit/s (status 0x%02X)", e->TX_PHY, e->Status);
    } else if (sub == HCI_LE_DATA_LENGTH_CHANGE_SUBEVT_CODE) {
      const hci_le_data_length_change_event_rp0 *e = (const hci_le_data_length_change_event_rp0 *)data;
      log_printf("BLE: packet length %u/%u bytes", e->MaxTxOctets, e->MaxRxOctets);
    }
  } else if (evt->evtcode == HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) {
    on_vendor_event((uint16_t)(evt->payload[0] | (evt->payload[1] << 8)), &evt->payload[2]);
  }
}

/* -------------------------------------------------------------- start-up */

static bool cpu2_boot(void)
{
  BOARD_ATOMIC(__HAL_RCC_HSEM_CLK_ENABLE());
  /* No LSE on this board: RF wake-up clock from HSE / 1024. */
  LL_RCC_SetRFWKPClockSource(LL_RCC_RFWKP_CLKSOURCE_HSE_DIV1024);
  /* CPU2 must not switch HSI48 off while USB uses it. */
  LL_HSEM_1StepLock(HSEM, 5);
  LL_C2_PWR_SetPowerMode(LL_PWR_MODE_SHUTDOWN);
  HAL_NVIC_SetPriority(IPCC_C1_RX_IRQn, 5, 0);
  HAL_NVIC_SetPriority(IPCC_C1_TX_IRQn, 5, 0);

  TL_Init();
  SHCI_TL_HciInitConf_t shci_conf = {.p_cmdbuffer = (uint8_t *)&system_cmd_buffer, .StatusNotCallBack = shci_status};
  shci_init(sys_user_evt, &shci_conf);
  TL_MM_Config_t mm = {
    .p_BleSpareEvtBuffer = ble_spare_evt_buffer,
    .p_SystemSpareEvtBuffer = system_spare_evt_buffer,
    .p_AsynchEvtPool = evt_pool,
    .AsynchEvtPoolSize = POOL_SIZE,
  };
  TL_MM_Init(&mm);
  cpu2_started = true;
  TL_Enable(); /* sets C2BOOT */

  uint32_t t0 = HAL_GetTick();
  while (!sys_ready && !dead && HAL_GetTick() - t0 < 3000U) {
    health_alive(HEALTH_BLE);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    if (sys_evt_pending) {
      sys_evt_pending = false;
      shci_user_evt_proc();
    }
  }
  return sys_ready;
}

/*
 * Address and name as the stock firmware gives them (identity.h): the phone and the BipLink app
 * see the same device before and after a firmware change. The name carries the first word of
 * the pilot's name of the profile chosen last.
 */
static void make_address(void)
{
  const uint32_t *uid64 = (const uint32_t *)UID64_BASE;
  identity_ble_address(own_addr, uid64[0], (uint8_t)uid64[1]);
  const config_t *c = config_get();
  const char *pilot = "";
  if (c->profile_last >= 1U && c->profile_last <= PROFILE_MAX && c->profile[c->profile_last - 1U].pilot[0]) {
    pilot = c->profile[c->profile_last - 1U].pilot;
  } else {
    for (unsigned i = 0; i < PROFILE_MAX && !pilot[0]; i++) {
      pilot = c->profile[i].pilot;
    }
  }
  if (c->ble_name[0]) {
    identity_device_name_with(dev_name, sizeof(dev_name), c->ble_name, uid64[0]);
  } else {
    identity_device_name(dev_name, sizeof(dev_name), pilot, uid64[0]);
  }
}

#define CHECK(call)                                                   \
  do {                                                                \
    tBleStatus ret_ = (call);                                         \
    if (ret_ != BLE_STATUS_SUCCESS) {                                 \
      log_printf("BLE: %s failed, status 0x%02X", #call, ret_);       \
      return false;                                                   \
    }                                                                 \
  } while (0)

static bool host_init(void)
{
  HCI_TL_HciInitConf_t hci_conf = {.p_cmdbuffer = (uint8_t *)&ble_cmd_buffer, .StatusNotCallBack = hci_status};
  hci_init(hci_user_evt, &hci_conf);

  uint8_t rsp_buf[TL_BLEEVT_CC_BUFFER_SIZE];
  TL_EvtPacket_t *rsp = (TL_EvtPacket_t *)rsp_buf;
  shci_send(SHCI_OPCODE_C2_BLE_INIT, sizeof(init_params), (uint8_t *)&init_params, rsp);
  uint8_t status = ((TL_CcEvt_t *)rsp->evtserial.evt.payload)->payload[0];
  if (status != 0) {
    log_printf("BLE: SHCI_C2_BLE_Init status 0x%02X", status);
    return false;
  }

  CHECK(hci_reset());
  static const uint8_t le_event_mask[8] = {0x5F, 0x08, 0, 0, 0, 0, 0, 0};
  hci_le_set_event_mask(le_event_mask);
  make_address();
  CHECK(aci_hal_write_config_data(CONFIG_DATA_PUBADDR_OFFSET, CONFIG_DATA_PUBADDR_LEN, own_addr));
  CHECK(aci_hal_set_tx_power_level(1, TX_POWER_LEVEL));
  CHECK(aci_gatt_init());
  uint16_t gap_svc, name_char, appearance_char;
  CHECK(aci_gap_init(GAP_PERIPHERAL_ROLE, 0, (uint8_t)strlen(dev_name), &gap_svc, &name_char, &appearance_char));
  CHECK(aci_gatt_update_char_value(gap_svc, name_char, 0, (uint8_t)strlen(dev_name), (const uint8_t *)dev_name));

  static const uint8_t svc_uuid[16] = FS_UUID(0x01);
  static const uint8_t cmd_uuid[16] = FS_UUID(0x02);
  static const uint8_t data_uuid[16] = FS_UUID(0x03);
  static const uint8_t status_uuid[16] = FS_UUID(0x04);
  Service_UUID_t s;
  Char_UUID_t c;
  memcpy(s.Service_UUID_128, svc_uuid, 16);
  CHECK(aci_gatt_add_service(UUID_TYPE_128, &s, PRIMARY_SERVICE, 10, &fs_svc));
  memcpy(c.Char_UUID_128, cmd_uuid, 16);
  CHECK(aci_gatt_add_char(fs_svc, UUID_TYPE_128, &c, VALUE_MAX, CHAR_PROP_WRITE, ATTR_PERMISSION_NONE,
                          GATT_NOTIFY_ATTRIBUTE_WRITE, 16, 1, &fs_cmd));
  memcpy(c.Char_UUID_128, data_uuid, 16);
  CHECK(aci_gatt_add_char(fs_svc, UUID_TYPE_128, &c, VALUE_MAX,
                          CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_NOTIFY, ATTR_PERMISSION_NONE,
                          GATT_NOTIFY_ATTRIBUTE_WRITE, 16, 1, &fs_data));
  memcpy(c.Char_UUID_128, status_uuid, 16);
  CHECK(aci_gatt_add_char(fs_svc, UUID_TYPE_128, &c, VALUE_MAX, CHAR_PROP_NOTIFY, ATTR_PERMISSION_NONE,
                          GATT_DONT_NOTIFY_EVENTS, 16, 1, &fs_status));
  if (!biplink_add_services()) {
    return false;
  }
  /* the remote only when it is switched on in the profile chosen last (as the name, at power on) */
  const config_t *cfg = config_get();
  uint8_t pl = cfg->profile_last >= 1U && cfg->profile_last <= PROFILE_MAX ? cfg->profile_last : 1U;
  if (cfg->profile[pl - 1U].bt_remote && !remote_add_service()) {
    log_printf("BLE: continuing without the remote");
  }
  return true;
}

static void start(void)
{
  if (!cpu2_started) {
    if (!cpu2_boot()) {
      log_printf("BLE: CPU2 not ready");
      dead = true;
      return;
    }
    log_printf("BLE: wireless stack %u.%u.%u, FUS %u.%u.%u", info.fw_major, info.fw_minor, info.fw_sub,
               info.fus_major, info.fus_minor, info.fus_sub);
  }
  if (!host_ready) {
    host_ready = host_init();
    if (!host_ready) {
      dead = true;
      return;
    }
  }
  adv_fail_tick = 0;
  advertising_reconcile();
  log_printf("BLE: advertising as %s", dev_name);
}

static void stop(void)
{
  for (unsigned i = 0; i < LINKS_MAX; i++) {
    if (links[i].handle != NO_LINK) {
      hci_disconnect(links[i].handle, 0x13);
    }
  }
  stop_advertising();
  info.running = false;
}

/* Console output (CONSOLE on) as status lines: a few notifications per round, the rest when the
 * stack has buffers again. */
static void console_tick(void)
{
  char pkt[VALUE_MAX];
  for (int i = 0; i < 4; i++) {
    size_t n = ble_console_take(pkt, payload_max());
    if (n == 0 || notify(fs_status, pkt, n) != BLE_STATUS_SUCCESS) {
      return;
    }
    ble_console_sent();
  }
}

static void ble_task(void *arg)
{
  (void)arg;
  for (;;) {
    health_alive(HEALTH_BLE);
    bool hurry = xf == XF_GET || xf == XF_STREAM || biplink_busy() || remote_busy() || ble_console_busy();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(hurry ? 2 : 100));
    if (dead) {
      continue;
    }
    if (setjmp(abort_jmp)) {
      abort_armed = false;
      continue;
    }
    abort_armed = true;
    if (sys_evt_pending) {
      sys_evt_pending = false;
      shci_user_evt_proc();
    }
    for (int i = 0; i < 16 && hci_evt_pending; i++) {
      hci_evt_pending = false;
      hci_user_evt_proc();
    }
    if (want_on && !host_ready) {
      start();
    } else if (!want_on && host_ready && (advertising || info.connected)) {
      stop();
    } else if (want_on && host_ready) {
      advertising_reconcile();
    }
    if (info.connected) {
      for (unsigned i = 0; i < LINKS_MAX; i++) {
        link_t *l = &links[i];
        if (l->handle != NO_LINK && !l->update_sent && HAL_GetTick() - l->tick > CONN_UPDATE_DELAY_MS) {
          /* 7.5-15 ms, no latency, 4 s supervision timeout: short round trips for the file service */
          l->update_sent = true;
          aci_l2cap_connection_parameter_update_req(l->handle, 6, 12, 0, 400);
        }
      }
      auth_tick();
      transfer_tick();
      biplink_tick();
      remote_tick();
      console_tick();
    }
    abort_armed = false;
  }
}

/* ------------------------------------------------------------------- API */

void ble_init(void)
{
  xTaskCreate(ble_task, "ble", 1024, NULL, 3, &task);
}

void ble_enable(void)
{
  want_on = true;
  wake();
}

void ble_disable(void)
{
  want_on = false;
  wake();
}

bool ble_remote_ready(void)
{
  return info.connected && remote_ready();
}

void ble_remote_press(void)
{
  remote_press();
  wake();
}

void ble_get_info(ble_info_t *out)
{
  *out = info;
}

const char *ble_name(void)
{
  return dev_name;
}
