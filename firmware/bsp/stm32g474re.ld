/* Linker script for the STM32G474RE on the NUCLEO-G474RE.
 *
 * RAM is SRAM1 + SRAM2 only (96 KB from 0x20000000): every DMA buffer lives there.
 * The 32 KB CCM SRAM (0x10000000) is used for the stack and for CPU-only work buffers
 * (section .ccmram), so nothing a DMA channel touches can ever land in it.
 */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM     (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
  CCMRAM  (xrw) : ORIGIN = 0x10000000, LENGTH = 32K
}

_estack = ORIGIN(CCMRAM) + LENGTH(CCMRAM);
_Min_Stack_Size = 0x2000;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
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

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array :
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
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
    PROVIDE(end = .);
    PROVIDE(_end = .);
  } >RAM

  /* CPU-only buffers: not zeroed by the start-up code. */
  .ccmram (NOLOAD) :
  {
    . = ALIGN(8);
    *(.ccmram)
    *(.ccmram*)
    . = ALIGN(8);
  } >CCMRAM

  /* Fails the link if the stack no longer fits above the CCM buffers. */
  ._stack (NOLOAD) :
  {
    . = ALIGN(8);
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >CCMRAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
