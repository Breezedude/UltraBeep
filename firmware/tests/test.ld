/* Memory layout for the logic tests in the Unicorn emulator (no start-up code). */
ENTRY(test_main)

MEMORY
{
  ROM (rwx) : ORIGIN = 0x08000000, LENGTH = 960K
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 192K
}

SECTIONS
{
  .text : { *(.text*) *(.rodata*) } > ROM
  /* Data stays where it is loaded: the emulator maps ROM writable, no copy needed. */
  .data : { *(.data*) } > ROM
  .bss (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) __bss_end__ = .; } > RAM
  end = .;
  /DISCARD/ : { *(.ARM.exidx*) }
}
