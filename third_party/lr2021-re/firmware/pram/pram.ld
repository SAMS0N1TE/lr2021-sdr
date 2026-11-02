/* PRAM image: everything packed from 0x101000 (sweep.o first, so its header is at the start).
   .bss isn't uploaded: keep it empty (build.sh checks), put initialised data in .data instead. */
ENTRY(_start)
SECTIONS
{
  . = 0x101000;
  .text   : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(.bss*) *(COMMON) }
}
