/* The bootloader copies the flat kernel image to physical 0x10000 and jumps
   straight to its first byte, so _start has to be at the very front. */

ENTRY(_start)

SECTIONS
{
    . = 0x00010000;

    .text ALIGN(16) :
    {
        *(.text.boot)       /* kernel/entry.asm must come first */
        *(.text*)
    }

    .rodata ALIGN(16) :
    {
        *(.rodata*)
    }

    .data ALIGN(16) :
    {
        *(.data*)
    }

    /* End of the flat binary: everything past here is .bss, which is not
       present on disk. The installer writes exactly this range out. */
    . = ALIGN(16);
    __image_end = .;

    __bss_start = .;
    .bss :
    {
        *(COMMON)
        *(.bss*)
    }
    . = ALIGN(16);
    __bss_end = .;

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
