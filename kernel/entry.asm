; -----------------------------------------------------------------------------
;  Kernel entry stub. The bootloader jumps here in 32-bit protected mode with a
;  flat GDT already loaded. We give the kernel a real stack, clear .bss (which
;  is not present in the flat binary on disk) and hand control to kmain().
; -----------------------------------------------------------------------------
[BITS 32]

section .text.boot
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    cli
    mov     esp, stack_top
    mov     ebp, esp

    ; zero the .bss section
    mov     edi, __bss_start
    mov     ecx, __bss_end
    sub     ecx, edi
    xor     eax, eax
    rep     stosb

    push    eax                     ; null return address for stack traces
    call    kmain

.hang:
    cli
    hlt
    jmp     .hang

section .bss
align 16
stack_bottom:
    resb 32768                      ; 32 KiB kernel stack
stack_top:
