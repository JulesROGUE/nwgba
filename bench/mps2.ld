/* QEMU mps2-an500: 4 MB at 0 for code, 4 MB of RAM, and 16 MB at 0x60000000
 * where the game ROM is put (the calculator has it in flash) */
MEMORY {
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 4M
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
  GAME (r) : ORIGIN = 0x60000000, LENGTH = 16M
}
ENTRY(Reset_Handler)
SECTIONS {
  .text : {
    KEEP(*(.vectors))
    . = ALIGN(32);
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
  } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  .game : { KEEP(*(.game)) } > GAME
  _sidata = LOADADDR(.data);
  .data : {
    _sdata = .;
    *(.data .data.*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH
  .bss : {
    _sbss = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
