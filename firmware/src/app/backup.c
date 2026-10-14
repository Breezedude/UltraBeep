#include "backup.h"
#include "board.h"

#include "stm32wbxx_hal.h"

#define FLAGS_MAGIC 0x52450000U /* "RE" */

static volatile uint32_t *reg_addr(unsigned n)
{
  return &RTC->BKP0R + n;
}

void backup_enable_write(void)
{
  BOARD_ATOMIC(__HAL_RCC_RTCAPB_CLK_ENABLE());
  HAL_PWR_EnableBkUpAccess();
}

uint32_t backup_read(backup_reg_t reg)
{
  return *reg_addr((unsigned)reg);
}

void backup_write(backup_reg_t reg, uint32_t value)
{
  backup_enable_write();
  *reg_addr((unsigned)reg) = value;
}

bool backup_get_flag(uint32_t flag)
{
  uint32_t v = backup_read(BACKUP_FLAGS);
  return (v & 0xFFFF0000U) == FLAGS_MAGIC && (v & flag) != 0U;
}

void backup_set_flag(uint32_t flag, bool on)
{
  uint32_t v = backup_read(BACKUP_FLAGS);
  if ((v & 0xFFFF0000U) != FLAGS_MAGIC) {
    v = FLAGS_MAGIC;
  }
  v = on ? (v | flag) : (v & ~flag);
  backup_write(BACKUP_FLAGS, v);
}

uint32_t backup_bootloader_report(void)
{
  return *reg_addr(14);
}

uint32_t backup_wakeup_flags(void)
{
  return *reg_addr(17);
}

uint32_t backup_reset_flags(void)
{
  return *reg_addr(18);
}

bool backup_report_valid(void)
{
  return *reg_addr(19) == 0x00BAD455U;
}
