/*
 * Linker script for the host-side test of the OpenBootloader USB module.
 * The code under test is built for Cortex-M33 (ARMv8-M mainline, same integer ISA as the Cortex-M55 target) and run by the Unicorn CPU emulator;
 * the addresses only need to be valid, readable and writable in the emulator.
 */
MEMORY
{
  CODE (rx)  : ORIGIN = 0x00010000, LENGTH = 2M
  DATA (rwx) : ORIGIN = 0x20000000, LENGTH = 16M
}
ENTRY(Reset_Handler)
_estack = ORIGIN(DATA) + LENGTH(DATA);

SECTIONS
{
  .text :
  {
    KEEP(*(.isr_vector))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > CODE

  .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > DATA AT > CODE
  .bss  : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > DATA
  ._user_heap_stack : { . = ALIGN(8); PROVIDE(end = .); PROVIDE(_end = .); . = . + 0x4000; } > DATA

  _sidata = LOADADDR(.data);
}
