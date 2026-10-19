#include "gps.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "stream_buffer.h"
#include "task.h"

#include "backup.h"
#include "board.h"
#include "health.h"
#include "log.h"
#include "nmea.h"
#include "rawlog.h"
#include "rtc_time.h"
#include "system.h"

#define GOOD_SNR         30U   /* dB-Hz */
#define SAT_TIMEOUT_MS   3000U
#define MAX_SATS         64U
#define TIME_TRUST_S     3600U /* stock: warm start only within 1 h of the last time update */
#define FIX_LOST_MS      5000U
#define PLACEHOLDER_MS   500U   /* an invalid epoch this soon after a valid one: the module skipped a cycle */
#define SILENT_MS        10000U /* no NMEA sentence this long while running: the module is restarted */
#define POSITION_SAVE_MS 60000U
#define BAUD_DEFAULT     115200U
#define OFF_MIN_MS       500U  /* a restart keeps the module off this long: it must forget the baud rate */
/* Only a cycle faster than this needs more than the factory 115200 baud: 5 Hz with GGA, RMC, GSA and GSV of
 * all systems is 1.3 kB/s measured (2026-10-16), a ninth of 115200 baud. Since 10 Hz is gone no cycle does,
 * so the module keeps its factory rate: should it restart itself, it talks at the rate we listen at (the
 * stock firmware, 115200 at 1 Hz, had no gap in 50 h of flights on the v3 in 2026; ours at 460800 had a
 * silence every 100 s or so in the air). */
#define FAST_UART_BELOW_MS 200U
/* Diagnostics in the raw log (the v3's silences in flight): */
#define QUIET_REPORT_MS  2000U  /* no sentence this long: what the UART received meanwhile */
#define STATS_MS         60000U /* the UART counters this often */
#define SLOW_RMC_MS      900U   /* sentences this far apart at a faster cycle: the module fell back to 1 Hz */
#define SLOW_RMC_RUN     3U     /* ... this many times in a row */
/* The module restarting itself (or going silent) this often at 5 Hz in one power-on: 1 Hz from then on. The v3
 * (board UltraBip_v1.0) restarted every ~50 s at 5 Hz in flight once warm, and ran 38 min clean at 1 Hz; the v4
 * (v1.1) never restarted at 5 Hz. One restart can be chance, the second is a pattern. */
#define RATE_FALLBACK_RESTARTS 2U

typedef enum { CMD_START, CMD_STOP, CMD_SEND, CMD_BOOT_PROBE } cmd_type_t;

typedef struct {
  uint8_t type;
  uint8_t mode;
  char text[48];
} cmd_t;

typedef struct {
  char talker[2];
  uint8_t prn;
  uint8_t snr;
  uint32_t tick;
} sat_t;

static UART_HandleTypeDef huart1;
static StreamBufferHandle_t rx;
static QueueHandle_t commands;
static gps_status_t st; /* written by the GPS task only */
static volatile bool monitor;
static volatile uint16_t want_cycle_ms = 200U; /* gps_set_rate */
static volatile uint8_t want_systems = GPS_SYSTEMS_ALL; /* gps_set_systems */
static volatile uint32_t overruns;
static uint32_t detail_second = UINT32_MAX;    /* second of day whose GSA/GSV go into the raw log */
static bool detail;
static sat_t sats[MAX_SATS];
static uint8_t view_by_talker[6];
static char line_buf[160];
static size_t line_len;
static const char *expected;
static bool expected_seen;
static char expected_err[16]; /* "[NAME] Err": the module refused the command */
static bool expected_failed;
static uint8_t gsa_best;      /* best fix type of the GSA sentences since the last GGA */
static uint32_t fix_lost_tick;
static uint32_t last_nmea_tick; /* last sentence with a good checksum (the silence watchdog) */
/* the UART counters at the last good sentence and the first bytes after it: what a silence consists of */
static uint32_t line_rx_bytes, line_rx_errors, line_printable, line_overruns, line_bad;
static uint8_t after_line[16];
static uint8_t after_line_n;
static uint32_t quiet_reported; /* tick of the last report of the current quiet spell, 0 = none */
static uint32_t stats_tick;
static uint32_t last_rmc_tick, slow_rmc_ms;
static uint8_t slow_rmc_run;
static uint32_t restarts;       /* restarts by the silence watchdog since power on */
static uint8_t fast_failures;   /* restarts and silences of the module at the fast cycle since power on */
static volatile bool rate_fallback_new; /* it ran into RATE_FALLBACK_RESTARTS: 1 Hz now, gps_take_rate_fallback tells */
static uint32_t last_position_save;
static uint32_t power_off_tick;
static bool was_powered;
static bool rtc_synced_this_session;
static nmea_epoch_t epoch;
static gps_fix_t fix; /* last completed epoch, copied under a critical section */

static const char *const talkers[6] = {"GP", "GL", "GA", "GB", "BD", "QZ"};

const char *gps_state_name(gps_state_t state)
{
  switch (state) {
  case GPS_OFF: return "off";
  case GPS_STARTING: return "starting";
  case GPS_SEARCHING: return "searching";
  case GPS_FIX: return "3D fix";
  default: return "?";
  }
}

const char *gps_start_name(gps_start_t mode)
{
  switch (mode) {
  case GPS_START_AUTO: return "auto";
  case GPS_START_COLD: return "cold";
  case GPS_START_TIME: return "time";
  default: return "?";
  }
}

static uint32_t since_power(void)
{
  uint32_t t = HAL_GetTick() - st.power_on_tick;
  return t ? t : 1U;
}

/* ---- UART ---- */

void HAL_UART_MspInit(UART_HandleTypeDef *h)
{
  if (h->Instance != USART1) {
    return;
  }
  BOARD_ATOMIC(__HAL_RCC_USART1_CLK_ENABLE());
  GPIO_InitTypeDef init = {0};
  init.Mode = GPIO_MODE_AF_PP;
  init.Pull = GPIO_PULLUP;
  init.Speed = GPIO_SPEED_FREQ_LOW;
  init.Alternate = GPIO_AF7_USART1;
  init.Pin = GPS_TX_PIN;
  board_gpio_init(GPS_TX_PORT, &init);
  init.Pin = GPS_RX_PIN;
  board_gpio_init(GPS_RX_PORT, &init);
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *h)
{
  if (h->Instance != USART1) {
    return;
  }
  __HAL_RCC_USART1_CLK_DISABLE();
  /* No level on the TX line while the module is unpowered (back-powering). */
  board_gpio_deinit(GPS_TX_PORT, GPS_TX_PIN);
  board_gpio_deinit(GPS_RX_PORT, GPS_RX_PIN);
}

static volatile uint32_t rx_bytes, rx_errors;
static volatile uint32_t printable_bytes; /* 0x20..0x7E: text at the right rate, noise at a wrong one */

void USART1_IRQHandler(void)
{
  uint32_t isr = USART1->ISR;
  if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) {
    if (isr & USART_ISR_ORE) {
      overruns++;
    }
    if (isr & (USART_ISR_FE | USART_ISR_NE)) {
      rx_errors++;
    }
    USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
  }
  /* the receive FIFO holds up to 8 bytes: take all of them at once */
  uint8_t b[8];
  size_t n = 0;
  while ((USART1->ISR & USART_ISR_RXNE_RXFNE) && n < sizeof(b)) {
    b[n++] = (uint8_t)USART1->RDR;
  }
  if (n) {
    rx_bytes += n;
    for (size_t i = 0; i < n; i++) {
      if (b[i] >= 0x20U && b[i] < 0x7FU) {
        printable_bytes++;
      }
    }
    BaseType_t woken = pdFALSE;
    if (xStreamBufferSendFromISR(rx, b, n, &woken) < n) {
      overruns++; /* the GPS task did not keep up */
    }
    portYIELD_FROM_ISR(woken);
  }
}

/* Also for a new baud rate while open: HAL_UART_Init leaves the pins and the interrupt enable alone. */
static void uart_config(uint32_t baud)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = baud;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  HAL_UART_Init(&huart1);
  /* 460800 baud: a byte every 22 us; the receive FIFO gives the interrupt 8 bytes of time */
  HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_8);
  HAL_UARTEx_EnableFifoMode(&huart1);
  line_len = 0;
}

static void uart_open(uint32_t baud)
{
  uart_config(baud);
  xStreamBufferReset(rx);
  HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  SET_BIT(USART1->CR1, USART_CR1_RXNEIE_RXFNEIE);
}

static void uart_close(void)
{
  HAL_NVIC_DisableIRQ(USART1_IRQn);
  HAL_UART_DeInit(&huart1);
}

static void send_cmd(const char *fmt, ...)
{
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2U, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  size_t len = (size_t)n < sizeof(buf) - 2U ? (size_t)n : sizeof(buf) - 3U;
  buf[len++] = '\r';
  buf[len++] = '\n';
  HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, 100);
  if (monitor) {
    console_printf("> %.*s", (int)len, buf);
  }
}

/* ---- NMEA ---- */

static bool checksum_ok(const char *s)
{
  const char *star = strchr(s, '*');
  if (!star || star == s) {
    return false;
  }
  uint8_t cs = 0;
  for (const char *p = s + 1; p < star; p++) {
    cs ^= (uint8_t)*p;
  }
  return strtoul(star + 1, NULL, 16) == cs;
}

/* Splits the sentence (without checksum) into fields; returns the count. */
static int split_fields(char *s, char **f, int max)
{
  char *star = strchr(s, '*');
  if (star) {
    *star = 0;
  }
  int n = 0;
  f[n++] = s;
  for (char *p = s; *p && n < max; p++) {
    if (*p == ',') {
      *p = 0;
      f[n++] = p + 1;
    }
  }
  return n;
}

static int talker_index(const char *t)
{
  for (int i = 0; i < 6; i++) {
    if (t[0] == talkers[i][0] && t[1] == talkers[i][1]) {
      return i;
    }
  }
  return -1;
}

static void update_good_count(void)
{
  uint32_t now = HAL_GetTick();
  uint8_t good = 0;
  for (unsigned i = 0; i < MAX_SATS; i++) {
    if (sats[i].prn && now - sats[i].tick < SAT_TIMEOUT_MS && sats[i].snr >= GOOD_SNR) {
      good++;
    }
  }
  st.sats_good = good;
  uint8_t view = 0;
  for (int i = 0; i < 6; i++) {
    view = (uint8_t)(view + view_by_talker[i]);
  }
  st.sats_view = view;
  if (good >= 1 && !st.t_first_signal) {
    st.t_first_signal = since_power();
    log_printf("GPS: first satellite >= %u dB-Hz after %lu ms", GOOD_SNR, (unsigned long)st.t_first_signal);
  }
  if (good >= 4 && !st.t_four_good) {
    st.t_four_good = since_power();
    log_printf("GPS: 4 satellites >= %u dB-Hz after %lu ms", GOOD_SNR, (unsigned long)st.t_four_good);
  }
}

static void store_sat(const char *talker, uint8_t prn, uint8_t snr)
{
  uint32_t now = HAL_GetTick();
  int free_slot = -1;
  int oldest = 0;
  for (unsigned i = 0; i < MAX_SATS; i++) {
    if (sats[i].prn == prn && sats[i].talker[0] == talker[0] && sats[i].talker[1] == talker[1]) {
      sats[i].snr = snr;
      sats[i].tick = now;
      return;
    }
    if (free_slot < 0 && (!sats[i].prn || now - sats[i].tick > SAT_TIMEOUT_MS)) {
      free_slot = (int)i;
    }
    if (sats[i].tick < sats[oldest].tick) {
      oldest = (int)i;
    }
  }
  sat_t *s = &sats[free_slot >= 0 ? free_slot : oldest];
  s->talker[0] = talker[0];
  s->talker[1] = talker[1];
  s->prn = prn;
  s->snr = snr;
  s->tick = now;
}

static void on_gsv(const char *talker, char **f, int n)
{
  if (n < 4) {
    return;
  }
  int ti = talker_index(talker);
  if (ti >= 0) {
    view_by_talker[ti] = (uint8_t)atoi(f[3]);
  }
  for (int k = 4; k + 3 < n; k += 4) {
    int prn = atoi(f[k]);
    if (prn > 0 && prn < 256) {
      store_sat(talker, (uint8_t)prn, (uint8_t)atoi(f[k + 3]));
    }
  }
  update_good_count();
}

void gps_print_sats(void)
{
  /* read while the GPS task may update an entry: one odd line at worst */
  uint32_t now = HAL_GetTick();
  uint8_t order[MAX_SATS];
  unsigned n = 0;
  for (unsigned i = 0; i < MAX_SATS; i++) {
    if (sats[i].prn && now - sats[i].tick < SAT_TIMEOUT_MS) {
      order[n++] = (uint8_t)i;
    }
  }
  for (unsigned i = 1; i < n; i++) { /* strongest first */
    uint8_t k = order[i];
    unsigned j = i;
    while (j && sats[order[j - 1U]].snr < sats[k].snr) {
      order[j] = order[j - 1U];
      j--;
    }
    order[j] = k;
  }
  unsigned heard = 0, sum4 = 0, c25 = 0, c30 = 0, c35 = 0, c40 = 0;
  for (unsigned i = 0; i < n; i++) {
    unsigned s = sats[order[i]].snr;
    if (s) {
      sum4 += heard < 4U ? s : 0U;
      heard++;
    }
    c25 += s >= 25U;
    c30 += s >= 30U;
    c35 += s >= 35U;
    c40 += s >= 40U;
  }
  console_printf("GPS satellites of the last %u s: %u heard of %u listed (%u in view)\r\n", SAT_TIMEOUT_MS / 1000U,
                 heard, n, st.sats_view);
  char rest[120];
  size_t len = 0;
  rest[0] = 0;
  for (unsigned i = 0; i < n; i++) {
    const sat_t *s = &sats[order[i]];
    if (s->snr) {
      char bar[26];
      unsigned b = s->snr / 2U < sizeof(bar) - 1U ? s->snr / 2U : sizeof(bar) - 1U;
      memset(bar, '#', b);
      bar[b] = 0;
      console_printf("  %c%c %3u  %2u dB-Hz  %s\r\n", s->talker[0], s->talker[1], s->prn, s->snr, bar);
    } else if (len + 8U < sizeof(rest)) {
      len += (size_t)snprintf(rest + len, sizeof(rest) - len, " %c%c%u", s->talker[0], s->talker[1], s->prn);
    }
  }
  if (heard < n) {
    console_printf("  not heard:%s\r\n", rest);
  }
  console_printf("C/N0: best 4 mean %u dB-Hz, >= 40: %u, >= 35: %u, >= 30: %u, >= 25: %u\r\n",
                 heard ? sum4 / (heard < 4U ? heard : 4U) : 0U, c40, c35, c30, c25);
}

static void on_gga(char **f, int n)
{
  if (n < 10) {
    return;
  }
  gsa_best = 0; /* a new epoch begins with GGA */
  st.quality = (uint8_t)atoi(f[6]);
  st.sats_used = (uint8_t)atoi(f[7]);
  st.hdop = nmea_parse_float(f[8]);
  if (st.quality > 0) {
    st.lat_e7 = nmea_coord_e7(f[2], f[3]);
    st.lon_e7 = nmea_coord_e7(f[4], f[5]);
    st.alt_m = nmea_parse_float(f[9]);
  }
}

static void on_rmc(char **f, int n)
{
  if (n < 10) {
    return;
  }
  const char *t = f[1];
  const char *d = f[9];
  if (strlen(t) >= 6 && strlen(d) == 6) {
    uint8_t hour = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
    uint8_t min = (uint8_t)((t[2] - '0') * 10 + (t[3] - '0'));
    uint8_t sec = (uint8_t)((t[4] - '0') * 10 + (t[5] - '0'));
    uint8_t day = (uint8_t)((d[0] - '0') * 10 + (d[1] - '0'));
    uint8_t month = (uint8_t)((d[2] - '0') * 10 + (d[3] - '0'));
    uint16_t year = (uint16_t)(2000 + (d[4] - '0') * 10 + (d[5] - '0'));
    /* the module's date before it knows the time is 06.01.80: no 2080 */
    if (year >= 2024 && year < 2080 && month >= 1 && month <= 12 && day >= 1) {
      st.unix_time = unix_from_civil(year, month, day, hour, min, sec);
      st.time_valid = true;
      if (!st.t_time) {
        st.t_time = since_power();
        log_printf("GPS: UTC time after %lu ms", (unsigned long)st.t_time);
      }
    }
  }
  if (f[2][0] == 'A') {
    st.speed_kmh = nmea_parse_float(f[7]) * 1.852f;
    st.course_deg = nmea_parse_float(f[8]);
  }
}

static void sync_rtc(void)
{
  if (!st.time_valid || rtc_synced_this_session) {
    return;
  }
  uint32_t gps = st.unix_time;
  if (rtc_time_valid()) {
    uint32_t last = rtc_time_last_sync();
    int32_t drift = (int32_t)(rtc_time_unix() - gps);
    uint32_t span = gps > last ? gps - last : 0U;
    int64_t ppm64 = span ? (int64_t)drift * 1000000 / (int64_t)span : 0;
    /* an RTC that lost its date (year 2000) gives rates beyond 32 bits */
    int32_t ppm = ppm64 > INT32_MAX ? INT32_MAX : ppm64 < INT32_MIN ? INT32_MIN : (int32_t)ppm64;
    log_printf("RTC: drift %+ld s over %lu s since the last sync (%ld ppm)", (long)drift, (unsigned long)span, (long)ppm);
    if (span >= RTC_DRIFT_MIN_SPAN_S) {
      rtc_time_set_drift(ppm);
    }
  }
  rtc_time_sync(gps);
  rtc_synced_this_session = true;
}

static void save_position(void)
{
  backup_write(BACKUP_LAST_LAT, (uint32_t)st.lat_e7);
  backup_write(BACKUP_LAST_LON, (uint32_t)st.lon_e7);
  last_position_save = HAL_GetTick();
}

static void on_gsa(char **f, int n)
{
  if (n < 3) {
    return;
  }
  uint8_t type = (uint8_t)atoi(f[2]);
  /* one GSA per satellite system: a system without satellites must not end the fix */
  if (type < 1 || type > 3 || type < gsa_best) {
    return;
  }
  gsa_best = type;
  st.fix_type = type;
  if (type >= 2 && !st.t_fix2d) {
    st.t_fix2d = since_power();
    log_printf("GPS: 2D fix after %lu ms", (unsigned long)st.t_fix2d);
  }
  if (type == 3) {
    fix_lost_tick = 0;
    if (st.state != GPS_FIX && st.state != GPS_OFF) {
      bool first = !st.t_fix3d;
      if (first) {
        st.t_fix3d = since_power();
      }
      st.state = GPS_FIX;
      log_printf("GPS: 3D fix %s %lu ms (start %s, time %s, %u used, 4 good at %lu ms, UTC at %lu ms)",
                 first ? "after" : "again after", (unsigned long)since_power(), gps_start_name(st.start_mode),
                 st.time_injected ? "given" : "not given", st.sats_used, (unsigned long)st.t_four_good,
                 (unsigned long)st.t_time);
      sync_rtc();
      save_position();
    } else if (HAL_GetTick() - last_position_save > POSITION_SAVE_MS) {
      save_position();
    }
  } else if (st.state == GPS_FIX) {
    if (!fix_lost_tick) {
      fix_lost_tick = HAL_GetTick();
    } else if (HAL_GetTick() - fix_lost_tick > FIX_LOST_MS) {
      st.state = GPS_SEARCHING;
      fix_lost_tick = 0;
      log_printf("GPS: fix lost");
    }
  }
}

/*
 * Raw log at 10 Hz: GGA and RMC of every epoch (position and velocity for the replay), GSA and GSV
 * only of the first epoch of each second (satellites change slowly). Epochs begin with GGA.
 */
static void log_line(const char *line)
{
  if (line[0] == '$' && strlen(line) > 13) {
    const char *type = line + 3;
    if (strncmp(type, "GGA", 3) == 0) {
      char hms[7];
      memcpy(hms, line + 7, 6);
      hms[6] = 0;
      uint32_t second = (uint32_t)strtoul(hms, NULL, 10);
      detail = second != detail_second;
      detail_second = second;
    } else if ((strncmp(type, "GSA", 3) == 0 || strncmp(type, "GSV", 3) == 0) && !detail) {
      return;
    }
  }
  rawlog_nmea(line);
}

static void handle_line(char *line)
{
  log_line(line);
  if (monitor) {
    console_printf("%s\r\n", line);
  }
  if (line[0] == '[') {
    if (expected && strncmp(line, expected, strlen(expected)) == 0) {
      expected_seen = true;
    } else if (expected_err[0] && strncmp(line, expected_err, strlen(expected_err)) == 0) {
      expected_failed = true;
    }
    return;
  }
  if (line[0] != '$' || strlen(line) < 7) {
    return;
  }
  if (!checksum_ok(line)) {
    st.bad_checksums++;
    return;
  }
  st.lines++;
  if (quiet_reported) {
    rawlog_event("gps talks again after %lu ms without a sentence", (unsigned long)(HAL_GetTick() - last_nmea_tick));
    quiet_reported = 0;
  }
  last_nmea_tick = HAL_GetTick();
  line_rx_bytes = rx_bytes;
  line_rx_errors = rx_errors;
  line_printable = printable_bytes;
  line_overruns = overruns;
  line_bad = st.bad_checksums;
  after_line_n = 0;
  if (st.lines > 100U) {
    restarts = 0; /* the module talks again: the backoff starts over next time */
  }
  if (!st.t_first_nmea) {
    st.t_first_nmea = since_power();
    log_printf("GPS: first NMEA after %lu ms", (unsigned long)st.t_first_nmea);
  }
  char talker[2] = {line[1], line[2]};
  const char *type = line + 3;
  char *f[48];
  int n = split_fields(line, f, 48);
  if (strncmp(type, "GSV", 3) == 0) {
    on_gsv(talker, f, n);
  } else if (strncmp(type, "GGA", 3) == 0) {
    on_gga(f, n);
    nmea_epoch_gga(&epoch, f, n);
  } else if (strncmp(type, "RMC", 3) == 0) {
    /* one RMC per epoch: sentences a second apart at a 5 Hz cycle mean the module restarted with its defaults */
    uint32_t now = HAL_GetTick();
    if (st.state != GPS_STARTING && st.cycle_ms < 1000U && last_rmc_tick && now - last_rmc_tick >= SLOW_RMC_MS) {
      slow_rmc_ms = now - last_rmc_tick;
      if (slow_rmc_run < 255U) {
        slow_rmc_run++;
      }
    } else {
      slow_rmc_run = 0;
    }
    last_rmc_tick = now;
    on_rmc(f, n);
    gps_fix_t done;
    if (nmea_epoch_rmc(&epoch, f, n, &done)) {
      done.rx_ms = HAL_GetTick();
      if (!done.valid && fix.valid && done.rx_ms - fix.rx_ms < PLACEHOLDER_MS) {
        /* At 10 Hz with all satellite systems the module computes only every other epoch and
         * sends the others as "no fix": the last fix stays current, a real loss shows after
         * PLACEHOLDER_MS without a valid epoch. */
        st.placeholders++;
      } else {
        taskENTER_CRITICAL();
        fix = done;
        taskEXIT_CRITICAL();
      }
    }
  } else if (strncmp(type, "GSA", 3) == 0) {
    on_gsa(f, n);
    nmea_epoch_gsa(&epoch, f, n);
  }
}

static void process_bytes(const uint8_t *b, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    char c = (char)b[i];
    if (after_line_n < sizeof(after_line)) {
      after_line[after_line_n++] = b[i];
    }
    if (c == '\n') {
      if (line_len && line_buf[line_len - 1] == '\r') {
        line_len--;
      }
      line_buf[line_len] = 0;
      if (line_len) {
        handle_line(line_buf);
      }
      line_len = 0;
    } else if (line_len < sizeof(line_buf) - 1U) {
      line_buf[line_len++] = c;
    }
  }
}

/* Reads and processes module output for up to ms; returns early when `expect` was seen. */
static bool pump(uint32_t ms, const char *expect)
{
  uint8_t buf[64];
  uint32_t start = HAL_GetTick();
  expected = expect;
  expected_seen = false;
  expected_failed = false;
  while (HAL_GetTick() - start < ms && !(expect && expected_seen) && !expected_failed) {
    health_alive(HEALTH_GPS);
    size_t n = xStreamBufferReceive(rx, buf, sizeof(buf), pdMS_TO_TICKS(20));
    process_bytes(buf, n);
  }
  expected = NULL;
  expected_err[0] = 0;
  return expected_seen;
}

/* ---- control ---- */

static void reset_status(gps_start_t mode)
{
  memset(&st, 0, sizeof(st));
  memset(sats, 0, sizeof(sats));
  memset(view_by_talker, 0, sizeof(view_by_talker));
  st.start_mode = mode;
  st.cycle_ms = 1000U;
  st.baud = BAUD_DEFAULT;
  detail_second = UINT32_MAX;
  st.power_on_tick = HAL_GetTick();
  line_len = 0;
  fix_lost_tick = 0;
  rtc_synced_this_session = false;
  nmea_epoch_init(&epoch);
  taskENTER_CRITICAL();
  memset(&fix, 0, sizeof(fix));
  taskEXIT_CRITICAL();
}

static bool command_ms(const char *name, const char *args, const char *done, uint32_t ms)
{
  snprintf(expected_err, sizeof(expected_err), "[%s] Err", name);
  if (args) {
    send_cmd("@%s %s", name, args);
  } else {
    send_cmd("@%s", name);
  }
  bool ok = pump(ms, done);
  if (!ok) {
    log_printf("GPS: no '%s' after @%s%s%s", done, name, args ? " " : "", args ? args : "");
  }
  return ok;
}

static bool command(const char *name, const char *args, const char *done)
{
  return command_ms(name, args, done, 500);
}

/*
 * Satellite systems (@GNS bits: 0 GPS, 1 GLONASS, 2 SBAS, 3 QZSS L1C/A, 7 Galileo). The module
 * forgets them at power off; they can only be set while it is idle, before the start. Not every
 * combination is allowed on every chip and firmware, so the largest one the module accepts wins.
 * More satellites: a faster fix, fewer dropouts, a better HDOP.
 */
static void select_systems(void)
{
  static const struct {
    const char *mask;
    const char *name;
  } sets[] = {
    {"0x8F", "GPS+GLONASS+Galileo+SBAS+QZSS"},
    {"0x0F", "GPS+GLONASS+SBAS+QZSS"},
    {"0x03", "GPS+GLONASS"},
    {"0x01", "GPS"},
  };
  /* system_gpssystems_string: all systems the module accepts, or fewer so 10 Hz solutions keep up */
  unsigned first = want_systems == GPS_SYSTEMS_GPS_GLONASS ? 2U : want_systems == GPS_SYSTEMS_GPS ? 3U : 0U;
  st.systems = "GPS";
  for (unsigned i = first; i < sizeof(sets) / sizeof(sets[0]); i++) {
    if (command("GNS", sets[i].mask, "[GNS] Done")) {
      st.systems = sets[i].name;
      break;
    }
  }
  log_printf("GPS: satellite systems %s", st.systems);
}

/* Positioning cycle (idle only). 5 Hz: the IGC data records and the velocity; the 1 Hz users take whole seconds.
 * Not 10 Hz: there the module sent about 9 epochs per second but computed only 5 of them (2026-10-12). */
static void select_cycle(void)
{
  char args[16];
  uint16_t want = want_cycle_ms;
  snprintf(args, sizeof(args), "1 %u 0", want);
  /* 1 Hz is the module's default, but one still running at 5 Hz (set up again after a silence, or falling back
   * to 1 Hz) has to be told; refused, it keeps the default after a start */
  if (command("GSOP", args, "[GSOP] Done") || want == 1000U) {
    st.cycle_ms = want;
  }
  log_printf("GPS: %u fixes per second", 1000U / st.cycle_ms);
}

/*
 * 10 Hz with several systems is about 10 kB/s with GSV, close to the 11.5 kB/s of 115200 baud. The
 * module answers a refused rate at once at the old rate; an accepted one after 100 ms at the new
 * rate (RYS8839 guide; the same command family). Whatever happens, @VER shows which rate it uses.
 */
static const uint32_t scan_rates[] = {115200U, 460800U, 230400U, 921600U, 57600U, 38400U, 19200U, 9600U, 4800U};

/*
 * A module that restored a saved UART rate from its flash talks at that rate from power on and
 * ignores everything sent at 115200 (seen on 2026-10-09: @BUP after a 10 Hz session at 460800
 * baud, and the module came up at 460800 and 10 Hz from then on, for the stock firmware too).
 * Listens at every rate until sentences with a good checksum arrive; returns that rate, or 0.
 * The UART stays at the last rate tried.
 */
/* Listens at one rate; returns the sentences with a good checksum that arrived. */
static uint32_t listen_at(uint32_t rate, uint32_t ms, uint32_t *bytes, uint32_t *errors, uint32_t *printable)
{
  uart_config(rate);
  xStreamBufferReset(rx);
  rx_bytes = 0;
  rx_errors = 0;
  printable_bytes = 0;
  uint32_t lines = st.lines;
  pump(ms, NULL);
  *bytes = rx_bytes;
  *errors = rx_errors;
  *printable = printable_bytes;
  return st.lines - lines;
}

static uint32_t find_baud(void)
{
  /* the module starts talking about 4.3 s after power on: wait for its first bytes */
  uint32_t start = HAL_GetTick();
  while (rx_bytes == 0U && HAL_GetTick() - start < 7000U) {
    pump(100, NULL);
  }
  if (rx_bytes == 0U) {
    return 0;
  }
  for (unsigned pass = 0; pass < 2; pass++) {
    for (unsigned i = 0; i < sizeof(scan_rates) / sizeof(scan_rates[0]); i++) {
      uint32_t b, e, p;
      if (listen_at(scan_rates[i], 600, &b, &e, &p) > 0U) {
        return scan_rates[i];
      }
    }
  }
  return 0;
}

/*
 * Clears the backup data in the module's flash (@BUPC, idle state, automatic backup off): a
 * backup written while the module ran at 10 Hz and 460800 baud brings those settings back at
 * every power on, and no host that opens the UART at 115200 - the stock firmware included - ever
 * hears the module again. After this the module boots with its factory settings.
 */
static void clear_backup(void)
{
  log_printf("GPS: clearing the module's saved backup data");
  if (command_ms("BUPC", NULL, "[BUPC] Done", 5000)) {
    vTaskDelay(pdMS_TO_TICKS(1500)); /* the flash write finishes before anything else happens */
    log_printf("GPS: backup data cleared - the module starts with factory settings from the next power on");
  } else {
    log_printf("GPS: the module refused @BUPC");
  }
}

static bool select_baud(void)
{
  static const uint32_t rates[] = {460800U, 230400U};
  for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
    snprintf(expected_err, sizeof(expected_err), "[CSBR] Err");
    send_cmd("@CSBR %lu", (unsigned long)rates[i]);
    bool early = pump(50, "[CSBR] Done");
    if (expected_failed) {
      log_printf("GPS: %lu baud refused", (unsigned long)rates[i]);
      continue;
    }
    uart_config(rates[i]);
    if (early || !pump(400, "[CSBR] Done")) {
      if (!command("VER", NULL, "[VER] Done")) {
        uart_config(BAUD_DEFAULT);
        bool old = command("VER", NULL, "[VER] Done");
        log_printf("GPS: no answer at %lu baud, %s", (unsigned long)rates[i],
                   old ? "still at 115200" : "no answer at 115200 either");
        if (!old) {
          break; /* lost: no further tries, the next power on starts at 115200 again */
        }
        continue;
      }
    }
    st.baud = rates[i];
    break;
  }
  log_printf("GPS: %lu baud", (unsigned long)st.baud);
  return st.baud != BAUD_DEFAULT;
}

static void start_positioning(void)
{
  if (command_ms("GSR", NULL, "[GSR] Done", 1500)) {
    return;
  }
  /* should not happen: without a start the module would stay idle */
  if (!command_ms("GCD", NULL, "[GCD] Done", 1500)) {
    log_printf("GPS: the module did not start positioning");
  }
}

static void start_sequence(gps_start_t mode)
{
  if (was_powered && HAL_GetTick() - power_off_tick < OFF_MIN_MS) {
    vTaskDelay(pdMS_TO_TICKS(OFF_MIN_MS - (HAL_GetTick() - power_off_tick)));
  }
  reset_status(mode);
  st.state = GPS_STARTING;
  last_nmea_tick = HAL_GetTick();
  /* supply first, then the UART: no level on its RX line before it has power */
  board_gps_power(true);
  uart_open(BAUD_DEFAULT);
  log_printf("GPS: power on (%s start)", gps_start_name(mode));
  pump(1250, NULL);

  bool awake = false;
  for (int i = 0; i < 3 && !awake; i++) {
    awake = command("WUP", NULL, "[WUP] Done");
  }
  uint32_t saved_rate = 0;
  if (!awake) {
    saved_rate = find_baud();
    if (saved_rate) {
      st.baud = saved_rate;
      log_printf("GPS: the module talks at %lu baud from power on (settings restored from its flash)",
                 (unsigned long)saved_rate);
      awake = command("WUP", NULL, "[WUP] Done");
    } else {
      uart_config(BAUD_DEFAULT);
    }
  }
  /* it positions at 1 Hz on its own after power on; the settings need the idle state */
  bool idle = command_ms("GSTP", NULL, "[GSTP] Done", 1500);
  command("BSSL", "45", "[BSSL] Done"); /* 0x2D: GGA, GSA, GSV, RMC */
  /* No automatic backup: its write at the first fix (and hourly) would store the 10 Hz cycle and
   * the fast UART rate with the navigation data, and the module would boot that way (see
   * stop_now). The backup for the hot start is written at power off, with the factory settings
   * restored first. */
  command("ABUP", "0", "[ABUP] Done");
  if (saved_rate && idle) {
    clear_backup();
  }
  select_systems();
  select_cycle();
  if (st.cycle_ms < FAST_UART_BELOW_MS && !select_baud()) {
    /* a cycle this fast with GSV of several systems would fill 115200 baud: back to 1 Hz */
    command("GSOP", "1 1000 0", "[GSOP] Done");
    st.cycle_ms = 1000U;
    log_printf("GPS: 1 fix per second (no faster UART)");
  }

  /* The time only from an RTC that keeps running while the device is off and whose rate error was
   * measured and is plausible, corrected by it. A wrong time slows the first fix down: on 2026-10-12
   * the stock's HSE/32 RTC still showed the day of the last switch-off (it stands still in standby),
   * the module needed 377 s against 90 s without a time, with the same signal strengths. A rate
   * measured over a span with little time off says nothing about the next, longer time off. */
  bool give_time = false;
  uint32_t est = 0, age = 0;
  if (rtc_time_valid() && mode != GPS_START_COLD) {
    int32_t ppm = 0;
    bool known = rtc_time_drift(&ppm);
    bool trusted = rtc_time_estimate(&est, &age);
    give_time = mode == GPS_START_TIME || (rtc_time_runs_while_off() && trusted && age <= TIME_TRUST_S);
    if (!trusted) {
      est = rtc_time_unix();
      age = est - rtc_time_last_sync();
    }
    if (known) {
      log_printf("GPS: RTC %lu s after the last GPS sync, rate error %ld ppm (%s) -> %s", (unsigned long)age,
                 (long)ppm, rtc_time_source_name(), give_time ? "give time" : "no time");
    } else {
      log_printf("GPS: RTC %lu s after the last GPS sync, rate error not measured yet (%s) -> %s",
                 (unsigned long)age, rtc_time_source_name(), give_time ? "give time" : "no time");
    }
  }
  if (give_time) {
    uint16_t year;
    uint8_t month, day, hour, min, sec;
    civil_from_unix(est, &year, &month, &day, &hour, &min, &sec);
    char args[32];
    snprintf(args, sizeof(args), "%u %u %u %u %u %u", year, month, day, hour, min, sec);
    st.time_injected = command("GTIM", args, "[GTIM] Done");
  }
  if (idle) {
    start_positioning(); /* otherwise it still positions with its defaults */
  }
  st.state = GPS_SEARCHING;
  log_printf("GPS: %s, searching", awake ? "module answered" : "module did NOT answer");
  rawlog_event("gps started: %s, %lu baud, %u ms, %s, %s start%s", awake ? "answered" : "NO answer",
               (unsigned long)st.baud, st.cycle_ms, st.systems, gps_start_name(mode), st.time_injected ? ", time given" : "");
}

/* Back to 115200 baud (idle state): the rate every host starts with. */
static bool restore_default_baud(void)
{
  if (st.baud == BAUD_DEFAULT) {
    return true;
  }
  snprintf(expected_err, sizeof(expected_err), "[CSBR] Err");
  send_cmd("@CSBR %lu", (unsigned long)BAUD_DEFAULT);
  bool early = pump(50, "[CSBR] Done");
  if (expected_failed) {
    log_printf("GPS: 115200 baud refused");
    return false;
  }
  uart_config(BAUD_DEFAULT);
  if (!early) {
    pump(400, "[CSBR] Done");
  }
  if (!command("VER", NULL, "[VER] Done")) {
    log_printf("GPS: no answer at 115200 baud after @CSBR");
    uart_config(st.baud);
    return false;
  }
  st.baud = BAUD_DEFAULT;
  return true;
}

static void stop_now(void)
{
  if (st.state == GPS_OFF) {
    return;
  }
  if (st.state == GPS_FIX) {
    save_position();
  }
  /*
   * Backup for the next hot start, but only with the factory UART rate and cycle active: the
   * module's backup data also stores those, and a module that boots at 460800 baud / 10 Hz is
   * silent for every host that opens the UART at 115200 (the stock firmware included; both
   * "dead" modules of 2026-10-08/09 were exactly this). So: idle, 115200 baud, 1 Hz, then @BUP,
   * then a pause for the flash write. Only a module that talks gets the commands; a silent one
   * is switched off at once.
   */
  if (st.lines > 0U && HAL_GetTick() - last_nmea_tick < 5000U) {
    if (command_ms("GSTP", NULL, "[GSTP] Done", 800)) {
      bool defaults = restore_default_baud();
      if (command("GSOP", "1 1000 0", "[GSOP] Done")) {
        st.cycle_ms = 1000U;
      } else {
        defaults = false;
      }
      if (defaults) {
        if (command_ms("BUP", NULL, "[BUP] Done", 3000)) {
          vTaskDelay(pdMS_TO_TICKS(300)); /* the flash write finishes before the supply goes */
          log_printf("GPS: backup written at 115200 baud and 1 Hz (hot start next time)");
        }
      } else {
        log_printf("GPS: no backup - the module did not take the factory settings");
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  uart_close();
  board_gps_power(false);
  power_off_tick = HAL_GetTick();
  was_powered = true;
  st.state = GPS_OFF;
  log_printf("GPS: power off");
}

/* See gps_boot_probe. Runs in the GPS task, with the module off and on again. */
static void boot_probe(uint32_t baud, unsigned seconds)
{
  vTaskDelay(pdMS_TO_TICKS(1000));
  if (baud == 0U) {
    /* rate scan: which rate turns the module's output into sentences */
    reset_status(GPS_START_AUTO);
    st.state = GPS_STARTING;
    board_gps_power(true);
    uart_open(BAUD_DEFAULT);
    console_printf("GPS rate scan: power on, waiting for the module's first bytes, then 1.1 s at every standard rate "
                   "and 0.5 s at every rate of the fine grids (a module at 1 Hz fills only every other 0.5 s window)\r\n");
    rx_bytes = 0;
    uint32_t start = HAL_GetTick();
    while (rx_bytes == 0U && HAL_GetTick() - start < 8000U) {
      pump(100, NULL);
    }
    console_printf("  first bytes after %lu ms\r\n", (unsigned long)(HAL_GetTick() - st.power_on_tick));
    /* the standard rates, then a fine grid around 115200 and 460800 for a module whose clock is off */
    for (unsigned step = 0; step < 3; step++) {
      uint32_t from = step == 0 ? 0U : step == 1 ? 80000U : 380000U;
      uint32_t to = step == 0 ? 0U : step == 1 ? 150000U : 540000U;
      uint32_t inc = step == 1 ? 2500U : 10000U;
      unsigned count = step == 0 ? (unsigned)(sizeof(scan_rates) / sizeof(scan_rates[0])) : (unsigned)((to - from) / inc + 1U);
      for (unsigned i = 0; i < count; i++) {
        uint32_t rate = step == 0 ? scan_rates[i] : from + i * inc;
        uint32_t b, e, p;
        uint32_t good = listen_at(rate, step == 0 ? 1100U : 500U, &b, &e, &p);
        console_printf("  %6lu baud: %4lu bytes, %4lu framing errors, %3lu printable, %lu good sentences%s\r\n",
                       (unsigned long)rate, (unsigned long)b, (unsigned long)e, (unsigned long)p, (unsigned long)good,
                       good ? "  <== the module's rate" : "");
      }
    }
    uart_close();
    board_gps_power(false);
    power_off_tick = HAL_GetTick();
    was_powered = true;
    st.state = GPS_OFF;
    return;
  }
  reset_status(GPS_START_AUTO);
  st.state = GPS_STARTING;
  board_gps_power(true);
  uart_open(baud);
  console_printf("GPS boot probe at %lu baud for %u s: power on, a line end every 250 ms\r\n",
                 (unsigned long)baud, seconds);
  uint32_t until = HAL_GetTick() + seconds * 1000U;
  uint32_t next_send = HAL_GetTick() + 1000U; /* the module gets a second to boot first */
  uint32_t total = 0;
  bool prompt = false;
  uint8_t prev = 0;
  uint32_t lines_before = st.lines;
  uint32_t first_line_tick = 0;
  while (HAL_GetTick() < until) {
    health_alive(HEALTH_GPS);
    if (HAL_GetTick() >= next_send) {
      HAL_UART_Transmit(&huart1, (uint8_t *)"\r\n", 2, 100);
      next_send += 250U;
    }
    uint8_t buf[32];
    size_t n = xStreamBufferReceive(rx, buf, sizeof(buf), pdMS_TO_TICKS(20));
    if (n) {
      char hex[3 * sizeof(buf) + 1];
      char txt[sizeof(buf) + 1];
      for (size_t i = 0; i < n; i++) {
        snprintf(hex + 3 * i, 4, "%02x ", buf[i]);
        txt[i] = buf[i] >= 32 && buf[i] < 127 ? (char)buf[i] : '.';
        if (prev == 'H' && buf[i] == '>') {
          prompt = true;
        }
        prev = buf[i];
      }
      txt[n] = 0;
      console_printf("  +%5lu ms %-96s |%s|\r\n", (unsigned long)(HAL_GetTick() - st.power_on_tick), hex, txt);
      total += n;
      process_bytes(buf, n); /* counts the sentences with a good checksum */
      if (st.lines > lines_before && !first_line_tick) {
        first_line_tick = HAL_GetTick();
      }
    }
  }
  if (st.lines > lines_before) {
    uint32_t span = HAL_GetTick() - first_line_tick;
    console_printf("GPS boot probe: %lu good sentences in %lu.%lu s at %lu baud: the module boots with %s\r\n",
                   (unsigned long)(st.lines - lines_before), (unsigned long)(span / 1000U),
                   (unsigned long)(span % 1000U / 100U), (unsigned long)baud,
                   span > 1500U && (st.lines - lines_before) * 1000U / span > 12U ? "a fast cycle (10 Hz?)" : "1 Hz");
  }
  console_printf("GPS boot probe: %lu bytes received, %lu framing errors, boot loader prompt \"H>\" %s\r\n",
                 (unsigned long)total, (unsigned long)rx_errors, prompt ? "SEEN" : "not seen");
  uart_close();
  board_gps_power(false);
  power_off_tick = HAL_GetTick();
  was_powered = true;
  st.state = GPS_OFF;
}

/* The settings again, without switching the module off: it keeps what it knows and finds the fix in seconds. */
static bool resume_in_place(void)
{
  if (!command_ms("GSTP", NULL, "[GSTP] Done", 1500)) {
    return false;
  }
  command("BSSL", "45", "[BSSL] Done");
  command("ABUP", "0", "[ABUP] Done");
  select_systems();
  st.cycle_ms = 1000U;
  select_cycle();
  start_positioning();
  last_nmea_tick = HAL_GetTick();
  last_rmc_tick = 0;
  slow_rmc_run = 0;
  quiet_reported = 0;
  return true;
}

/*
 * The silence watchdog fired: does the module still answer, at our rate or at the factory rate (it restarted
 * itself and came up with its defaults)? An answering module is set up again in place; true then.
 */
static bool probe_and_resume(void)
{
  uint32_t baud = st.baud;
  bool answers = command("WUP", NULL, "[WUP] Done") || command("WUP", NULL, "[WUP] Done");
  if (!answers && baud != BAUD_DEFAULT) {
    uart_config(BAUD_DEFAULT);
    answers = command("WUP", NULL, "[WUP] Done");
    if (answers) {
      st.baud = BAUD_DEFAULT;
    } else {
      uart_config(baud);
    }
  }
  const char *what = !answers ? "no answer" : st.baud == baud ? "answers at our rate" : "answers at 115200 baud (it restarted itself)";
  rawlog_event("gps probe: %s", what);
  log_printf("GPS: probe: %s", what);
  if (!answers) {
    return false;
  }
  bool ok = resume_in_place();
  rawlog_event("gps %s without a power cycle", ok ? "set up again" : "could not be set up");
  log_printf("GPS: %s without a power cycle", ok ? "set up again" : "could not be set up");
  return ok;
}

/* No sentence for QUIET_REPORT_MS: what arrived since the last one, every QUIET_REPORT_MS while it lasts. */
static void quiet_report(void)
{
  uint32_t now = HAL_GetTick();
  if ((st.state != GPS_SEARCHING && st.state != GPS_FIX) || !last_nmea_tick || now - last_nmea_tick < QUIET_REPORT_MS ||
      (quiet_reported && now - quiet_reported < QUIET_REPORT_MS)) {
    return;
  }
  bool first = !quiet_reported;
  quiet_reported = now;
  rawlog_event("gps quiet %lu s: %lu B, %lu printable, %lu FE, %lu ORE, %lu bad, %lu mV",
               (unsigned long)((now - last_nmea_tick) / 1000U), (unsigned long)(rx_bytes - line_rx_bytes),
               (unsigned long)(printable_bytes - line_printable), (unsigned long)(rx_errors - line_rx_errors),
               (unsigned long)(overruns - line_overruns), (unsigned long)(st.bad_checksums - line_bad),
               (unsigned long)system_battery_mv());
  if (first) {
    char hex[3 * sizeof(after_line) + 1] = "";
    for (uint8_t i = 0; i < after_line_n; i++) {
      snprintf(hex + 3 * i, 4, "%02x ", after_line[i]);
    }
    rawlog_event("gps quiet, first bytes: %s", after_line_n ? hex : "none");
    log_printf("GPS: quiet for %lu ms, %lu bytes since the last sentence", (unsigned long)(now - last_nmea_tick),
               (unsigned long)(rx_bytes - line_rx_bytes));
  }
}

/*
 * The module failed at the fast cycle - it restarted itself, or went silent. After RATE_FALLBACK_RESTARTS of these
 * in one power-on it runs at 1 Hz (where it has run cleanly on every device so far): the setup that follows uses
 * that rate, and system.c stores the info flag so the next power-on starts at 1 Hz.
 */
static void fast_cycle_failed(void)
{
  if (st.cycle_ms >= 1000U || want_cycle_ms >= 1000U) {
    return;
  }
  if (++fast_failures < RATE_FALLBACK_RESTARTS) {
    return;
  }
  want_cycle_ms = 1000U;
  rate_fallback_new = true;
  rawlog_event("gps unstable at %u ms: %u restarts since power on, 1 Hz from now on", st.cycle_ms, fast_failures);
  log_printf("GPS: the module failed %u times at %u fixes per second - 1 Hz from now on", fast_failures, 1000U / st.cycle_ms);
}

/* The UART counters once a minute: the baseline the quiet reports compare with. */
static void stats_report(void)
{
  uint32_t now = HAL_GetTick();
  if ((st.state != GPS_SEARCHING && st.state != GPS_FIX) || now - stats_tick < STATS_MS) {
    return;
  }
  stats_tick = now;
  rawlog_event("gps stats: %lu ln, %lu bad, %lu B, %lu FE, %lu ORE, %lu skip, %lu Bd, %u ms, %u/%u sats",
               (unsigned long)st.lines, (unsigned long)st.bad_checksums, (unsigned long)rx_bytes,
               (unsigned long)rx_errors, (unsigned long)overruns, (unsigned long)st.placeholders,
               (unsigned long)st.baud, st.cycle_ms, st.sats_good, st.sats_view);
}

static void gps_task(void *arg)
{
  (void)arg;
  uint8_t buf[64];
  cmd_t c;
  for (;;) {
    health_alive(HEALTH_GPS);
    while (xQueueReceive(commands, &c, 0) == pdTRUE) {
      if (c.type == CMD_START) {
        stop_now();
        start_sequence((gps_start_t)c.mode);
      } else if (c.type == CMD_STOP) {
        stop_now();
      } else if (c.type == CMD_SEND && st.state != GPS_OFF) {
        send_cmd("%s", c.text);
      } else if (c.type == CMD_BOOT_PROBE) {
        stop_now();
        boot_probe((uint32_t)atol(c.text), c.mode);
      }
    }
    if (st.state == GPS_OFF) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    size_t n = xStreamBufferReceive(rx, buf, sizeof(buf), pdMS_TO_TICKS(100));
    process_bytes(buf, n);
    update_good_count();
    quiet_report();
    stats_report();
    if (slow_rmc_run >= SLOW_RMC_RUN && st.state != GPS_STARTING) {
      rawlog_event("gps sends every %lu ms instead of %u ms: it restarted itself", (unsigned long)slow_rmc_ms, st.cycle_ms);
      log_printf("GPS: sentences every %lu ms instead of %u ms - the module restarted itself, setting it up again",
                 (unsigned long)slow_rmc_ms, st.cycle_ms);
      slow_rmc_run = 0;
      fast_cycle_failed();
      bool ok = resume_in_place();
      rawlog_event("gps %s without a power cycle", ok ? "set up again" : "could not be set up");
    }
    /* A module that went silent (brown-out, a baud rate it forgot) is probed first and set up again in
     * place when it answers; otherwise switched off and started again. The wait doubles with every
     * restart in a row, up to 80 s. */
    uint32_t limit = SILENT_MS << (restarts < 3U ? restarts : 3U);
    if (st.state != GPS_OFF && st.state != GPS_STARTING && last_nmea_tick && HAL_GetTick() - last_nmea_tick > limit) {
      log_printf("GPS: no sentences for %lu s - probing the module", (unsigned long)(limit / 1000U));
      rawlog_event("gps silent for %lu s: probing", (unsigned long)(limit / 1000U));
      restarts++;
      fast_cycle_failed();
      if (!probe_and_resume()) {
        log_printf("GPS: restarting the module (power cycle)");
        rawlog_event("gps power cycle");
        gps_start_t mode = st.start_mode;
        stop_now();
        start_sequence(mode);
      }
      last_nmea_tick = HAL_GetTick();
    }
  }
}

/* ---- API ---- */

void gps_init(void)
{
  st.state = GPS_OFF;
  rx = xStreamBufferCreate(1024, 64);
  commands = xQueueCreate(4, sizeof(cmd_t));
  xTaskCreate(gps_task, "gps", 640, NULL, 3, NULL);
}

void gps_start(gps_start_t mode)
{
  cmd_t c = {CMD_START, (uint8_t)mode, ""};
  xQueueSend(commands, &c, 0);
}

void gps_stop(void)
{
  cmd_t c = {CMD_STOP, 0, ""};
  xQueueSend(commands, &c, 0);
}

void gps_set_rate(unsigned hz)
{
  want_cycle_ms = hz >= 5U ? 200U : 1000U;
}

bool gps_take_rate_fallback(void)
{
  bool fell = rate_fallback_new;
  rate_fallback_new = false;
  return fell;
}

void gps_set_systems(uint8_t systems)
{
  want_systems = systems;
}

void gps_send(const char *command)
{
  cmd_t c = {CMD_SEND, 0, ""};
  snprintf(c.text, sizeof(c.text), "%s", command);
  xQueueSend(commands, &c, 0);
}

void gps_set_monitor(bool on)
{
  monitor = on;
}

void gps_boot_probe(uint32_t baud, unsigned seconds)
{
  cmd_t c = {CMD_BOOT_PROBE, (uint8_t)(seconds > 60U ? 60U : seconds), ""};
  snprintf(c.text, sizeof(c.text), "%lu", (unsigned long)(baud != 0U && (baud < 1200U || baud > 921600U) ? 115200U : baud));
  xQueueSend(commands, &c, 0);
}

void gps_blast(unsigned seconds)
{
  if (st.state == GPS_OFF) {
    return;
  }
  static const uint8_t pattern[32] = {[0 ... 31] = 0x55}; /* 01010101: a 50 % duty cycle on the TX line */
  uint32_t until = HAL_GetTick() + seconds * 1000U;
  while (HAL_GetTick() < until) {
    HAL_UART_Transmit(&huart1, (uint8_t *)pattern, sizeof(pattern), 100);
    vTaskDelay(1);
  }
}

void gps_get(gps_status_t *s)
{
  taskENTER_CRITICAL();
  *s = st;
  s->overruns = overruns;
  s->restarts = restarts;
  s->alive = st.lines > 0U && HAL_GetTick() - last_nmea_tick < 5000U;
  s->rx_bytes = rx_bytes;
  s->rx_errors = rx_errors;
  taskEXIT_CRITICAL();
}

void gps_set_baud(uint32_t baud)
{
  if (st.state == GPS_OFF || baud < 1200U || baud > 921600U) {
    return;
  }
  uart_open(baud);
  st.baud = baud;
  st.lines = 0;
  st.bad_checksums = 0;
  rx_bytes = 0;
  rx_errors = 0;
  log_printf("GPS: listening at %lu baud", (unsigned long)baud);
}

void gps_probe_rx(bool *with_pull_up, bool *with_pull_down)
{
  GPIO_InitTypeDef init = {0};
  init.Pin = GPS_RX_PIN;
  init.Mode = GPIO_MODE_INPUT;
  init.Pull = GPIO_PULLUP;
  board_gpio_init(GPS_RX_PORT, &init);
  vTaskDelay(pdMS_TO_TICKS(5));
  *with_pull_up = HAL_GPIO_ReadPin(GPS_RX_PORT, GPS_RX_PIN) == GPIO_PIN_SET;
  init.Pull = GPIO_PULLDOWN;
  board_gpio_init(GPS_RX_PORT, &init);
  vTaskDelay(pdMS_TO_TICKS(5));
  *with_pull_down = HAL_GPIO_ReadPin(GPS_RX_PORT, GPS_RX_PIN) == GPIO_PIN_SET;
  if (st.state != GPS_OFF) {
    init.Mode = GPIO_MODE_AF_PP;
    init.Pull = GPIO_PULLUP;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    init.Alternate = GPIO_AF7_USART1;
    board_gpio_init(GPS_RX_PORT, &init);
  } else {
    board_gpio_deinit(GPS_RX_PORT, GPS_RX_PIN);
  }
}

void gps_get_fix(gps_fix_t *out)
{
  taskENTER_CRITICAL();
  *out = fix;
  taskEXIT_CRITICAL();
}
