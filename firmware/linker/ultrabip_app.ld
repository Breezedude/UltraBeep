/*
 * Linker script for custom UltraBip application images.
 *
 * The stock Stodeus bootloader occupies 0x08000000-0x0800EFFF and starts the
 * application at 0x0800F000. The 8 bytes at offset 0x13C (directly after the
 * 79-entry vector table) hold {image_len, cksum}; they are filled in by
 * tools/mkupdate.py, which also appends the same 8 bytes as trailer.
 *
 * RAM starts at 0x20000008 like in the stock image and ST's templates; the
 * first 8 bytes of SRAM1 are left untouched.
 */
ENTRY(Reset_Handler)

_estack = 0x20030000;
_Min_Heap_Size  = 0x800;
_Min_Stack_Size = 0x1000;

MEMORY
{
  /* Same span as the stock image (0x54000); the real limit is SFSA. */
  FLASH (rx)       : ORIGIN = 0x0800F000, LENGTH = 0x54000
  RAM1 (xrw)       : ORIGIN = 0x20000008, LENGTH = 0x2FFF8
  RAM_SHARED (xrw) : ORIGIN = 0x20030000, LENGTH = 10K
}

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
    ASSERT(. == ORIGIN(FLASH) + 0x13C, "vector table must end at offset 0x13C");
    /* Update header slot: image_len, cksum (written by mkupdate.py). */
    LONG(0xFFFFFFFF)
    LONG(0xFFFFFFFF)
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab (READONLY) : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM (READONLY) :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array (READONLY) :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array (READONLY) :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array (READONLY) :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } >RAM1 AT> FLASH

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
  } >RAM1

  /* STM32_WPAN transport layer state: zeroed together with .bss (_sbss.._ebss). */
  .wpan_bss (NOLOAD) :
  {
    *(SYSTEM_DRIVER_CONTEXT)
    *(BLE_DRIVER_CONTEXT)
    *(BLE_APP_CONTEXT)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM1

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM1

  .ARM.attributes 0 : { *(.ARM.attributes) }

  /*
   * CPU1 <-> CPU2 mailbox in SRAM2a. IPCCBR = 0 on this device: the mapping
   * table must start at 0x20030000. SRAM2a is secure (CPU2) from 0x20034800.
   */
  MAPPING_TABLE (NOLOAD) : { *(MAPPING_TABLE) } >RAM_SHARED
  MB_MEM1 (NOLOAD)       : { *(MB_MEM1) } >RAM_SHARED
  MB_MEM2 (NOLOAD)       : { _sMB_MEM2 = . ; *(MB_MEM2) ; _eMB_MEM2 = . ; } >RAM_SHARED
  /* The CubeWB startup copies MB_MEM2 from _siMB_MEM2: nothing to copy (copy onto itself). */
  _siMB_MEM2 = _sMB_MEM2;
  ASSERT(ADDR(MAPPING_TABLE) == 0x20030000, "mailbox mapping table must be at the start of SRAM2a")
}
