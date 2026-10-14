/*
 * Faults end in an immediate reset; the cause is kept in RTC backup
 * registers and logged at the next start. Because the device was switched
 * on, it resumes without the power-on gesture (system.c, BOOT_RESUME).
 */
#include "FreeRTOS.h"
#include "task.h"

#include "backup.h"
#include "board.h"
#include "faults.h"
#include "log.h"

__attribute__((noreturn)) static void fault_reset(uint32_t code, uint32_t detail, uint32_t pc)
{
  __disable_irq();
  backup_write(BACKUP_FAULT, (code << 24) | (detail & 0x00FFFFFFU));
  backup_write(BACKUP_FAULT_PC, pc);
  NVIC_SystemReset();
  for (;;) {
  }
}

void hard_fault_c(const uint32_t *frame)
{
  fault_reset(FAULT_HARD, SCB->CFSR & 0x00FFFFFFU, frame[6]);
}

__attribute__((naked)) void HardFault_Handler(void)
{
  __asm volatile("tst lr, #4      \n"
                 "ite eq          \n"
                 "mrseq r0, msp   \n"
                 "mrsne r0, psp   \n"
                 "b hard_fault_c  \n");
}

void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));

void fault_assert(const char *file, int line)
{
  (void)file;
  fault_reset(FAULT_ASSERT, (uint32_t)line, 0);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
  (void)task;
  uint32_t tag = ((uint32_t)(uint8_t)name[0] << 16) | ((uint32_t)(uint8_t)name[1] << 8) | (uint8_t)name[2];
  fault_reset(FAULT_STACK, tag, 0);
}

void vApplicationMallocFailedHook(void)
{
  fault_reset(FAULT_MALLOC, 0, 0);
}

void vApplicationIdleHook(void)
{
  __WFI();
}

void faults_log_last(void)
{
  uint32_t f = backup_read(BACKUP_FAULT);
  if (!f) {
    return;
  }
  static const char *const names[] = {"none", "hard fault", "assert", "stack overflow", "out of memory"};
  uint32_t code = f >> 24;
  log_printf("Last reset caused by %s (detail 0x%06lX, pc 0x%08lX)", code < 5 ? names[code] : "?",
             (unsigned long)(f & 0x00FFFFFFU), (unsigned long)backup_read(BACKUP_FAULT_PC));
  backup_write(BACKUP_FAULT, 0);
}
