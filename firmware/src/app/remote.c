#include "remote.h"

#include <string.h>

#include "ble_common.h"
#include "ble_core.h"
#include "board.h"
#include "log.h"

/* Boot keyboard: modifier byte, reserved byte, six key codes. */
static const uint8_t report_map[] = {
  0x05, 0x01,       /* Usage Page (Generic Desktop) */
  0x09, 0x06,       /* Usage (Keyboard) */
  0xA1, 0x01,       /* Collection (Application) */
  0x05, 0x07,       /*   Usage Page (Key Codes) */
  0x19, 0xE0,       /*   Usage Minimum (224) */
  0x29, 0xE7,       /*   Usage Maximum (231) */
  0x15, 0x00,       /*   Logical Minimum (0) */
  0x25, 0x01,       /*   Logical Maximum (1) */
  0x75, 0x01,       /*   Report Size (1) */
  0x95, 0x08,       /*   Report Count (8) */
  0x81, 0x02,       /*   Input (Data, Variable, Absolute): modifier byte */
  0x95, 0x01,       /*   Report Count (1) */
  0x75, 0x08,       /*   Report Size (8) */
  0x81, 0x01,       /*   Input (Constant): reserved byte */
  0x95, 0x06,       /*   Report Count (6) */
  0x75, 0x08,       /*   Report Size (8) */
  0x15, 0x00,       /*   Logical Minimum (0) */
  0x25, 0x65,       /*   Logical Maximum (101) */
  0x05, 0x07,       /*   Usage Page (Key Codes) */
  0x19, 0x00,       /*   Usage Minimum (0) */
  0x29, 0xE7,       /*   Usage Maximum (231) */
  0x81, 0x00,       /*   Input (Data, Array): key codes */
  0xC0,             /* End Collection */
};

#define REPORT_LEN 8

static uint16_t hid_svc, hid_info, hid_map, hid_cp, hid_report;
static bool added;
static volatile bool subscribed; /* the host enabled the report's notifications */
static volatile bool press_wanted;
static bool pressed;
static uint32_t press_tick;

static bool add_char16(uint16_t uuid16, uint8_t max_len, uint8_t props, uint8_t perms, uint8_t evt_mask,
                       uint8_t variable, uint16_t *handle)
{
  Char_UUID_t u;
  u.Char_UUID_16 = uuid16;
  return aci_gatt_add_char(hid_svc, UUID_TYPE_16, &u, max_len, props, perms, evt_mask, 16, variable, handle) ==
         BLE_STATUS_SUCCESS;
}

bool remote_add_service(void)
{
  /* Pairing without a PIN, bonded: the phone keeps the keys and reconnects as a keyboard. */
  if (aci_gap_set_io_capability(IO_CAP_NO_INPUT_NO_OUTPUT) != BLE_STATUS_SUCCESS ||
      aci_gap_set_authentication_requirement(BONDING, MITM_PROTECTION_NOT_REQUIRED, 0x01 /* SC supported */,
                                             0x00 /* no keypress notifications */, 7, 16,
                                             USE_FIXED_PIN_FOR_PAIRING_FORBIDDEN, 0,
                                             GAP_PUBLIC_ADDR) != BLE_STATUS_SUCCESS) {
    log_printf("BLE: remote: pairing settings failed");
    return false;
  }

  Service_UUID_t s;
  s.Service_UUID_16 = 0x1812; /* Human Interface Device */
  bool ok = aci_gatt_add_service(UUID_TYPE_16, &s, PRIMARY_SERVICE, 12, &hid_svc) == BLE_STATUS_SUCCESS;

  /* HID Information: bcdHID 1.11, no country code, normally connectable */
  static const uint8_t info[4] = {0x11, 0x01, 0x00, 0x02};
  ok = ok && add_char16(0x2A4A, sizeof(info), CHAR_PROP_READ, ATTR_PERMISSION_NONE, GATT_DONT_NOTIFY_EVENTS,
                        CHAR_VALUE_LEN_CONSTANT, &hid_info);
  ok = ok && aci_gatt_update_char_value(hid_svc, hid_info, 0, sizeof(info), info) == BLE_STATUS_SUCCESS;

  /* Report Map: reading it needs an encrypted link, which makes the phone pair */
  ok = ok && add_char16(0x2A4B, sizeof(report_map), CHAR_PROP_READ, ATTR_PERMISSION_ENCRY_READ,
                        GATT_DONT_NOTIFY_EVENTS, CHAR_VALUE_LEN_CONSTANT, &hid_map);
  ok = ok && aci_gatt_update_char_value(hid_svc, hid_map, 0, sizeof(report_map), report_map) == BLE_STATUS_SUCCESS;

  /* HID Control Point: suspend / exit suspend, nothing to do */
  ok = ok && add_char16(0x2A4C, 1, CHAR_PROP_WRITE_WITHOUT_RESP, ATTR_PERMISSION_NONE, GATT_NOTIFY_ATTRIBUTE_WRITE,
                        CHAR_VALUE_LEN_CONSTANT, &hid_cp);

  /* Report (input) with its Report Reference: report id 0, input */
  static const uint8_t empty[REPORT_LEN] = {0};
  ok = ok && add_char16(0x2A4D, REPORT_LEN, CHAR_PROP_READ | CHAR_PROP_NOTIFY, ATTR_PERMISSION_NONE,
                        GATT_NOTIFY_ATTRIBUTE_WRITE, CHAR_VALUE_LEN_CONSTANT, &hid_report);
  ok = ok && aci_gatt_update_char_value(hid_svc, hid_report, 0, REPORT_LEN, empty) == BLE_STATUS_SUCCESS;
  static const uint8_t reference[2] = {0x00, 0x01};
  Char_Desc_Uuid_t du;
  du.Char_UUID_16 = 0x2908;
  uint16_t ref_desc;
  ok = ok && aci_gatt_add_char_desc(hid_svc, hid_report, UUID_TYPE_16, &du, sizeof(reference), sizeof(reference),
                                    reference, ATTR_PERMISSION_NONE, ATTR_ACCESS_READ_ONLY, GATT_DONT_NOTIFY_EVENTS,
                                    16, CHAR_VALUE_LEN_CONSTANT, &ref_desc) == BLE_STATUS_SUCCESS;
  added = ok;
  log_printf(ok ? "BLE: remote control (keyboard) ready" : "BLE: remote control service failed");
  return ok;
}

void remote_on_connect(void)
{
  subscribed = false;
  pressed = false;
}

void remote_on_disconnect(void)
{
  subscribed = false;
  pressed = false;
  press_wanted = false;
}

bool remote_on_attribute_modified(uint16_t attr_handle, const uint8_t *data, uint16_t len)
{
  if (!added) {
    return false;
  }
  if (attr_handle == hid_report + 2U) { /* the report's client configuration */
    subscribed = len >= 1 && (data[0] & 0x01U);
    log_printf("BLE: remote: key reports %s", subscribed ? "on" : "off");
    return true;
  }
  return attr_handle > hid_svc && attr_handle <= hid_report + 3U;
}

static void send_report(uint8_t key)
{
  uint8_t report[REPORT_LEN] = {0};
  report[2] = key;
  tBleStatus st = aci_gatt_update_char_value(hid_svc, hid_report, 0, REPORT_LEN, report);
  if (st != BLE_STATUS_SUCCESS) {
    log_printf("BLE: remote: key report failed 0x%02X", st);
  }
}

void remote_tick(void)
{
  if (!added) {
    return;
  }
  if (pressed) {
    if (HAL_GetTick() - press_tick >= REMOTE_PRESS_MS) {
      send_report(0);
      pressed = false;
    }
  } else if (press_wanted) {
    press_wanted = false;
    if (subscribed) {
      send_report(REMOTE_KEY);
      pressed = true;
      press_tick = HAL_GetTick();
    }
  }
}

bool remote_busy(void)
{
  return pressed || press_wanted;
}

bool remote_ready(void)
{
  return added && subscribed;
}

void remote_press(void)
{
  press_wanted = true;
}
