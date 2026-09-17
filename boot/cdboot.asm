; =============================================================================
;  LightningOS - El Torito CD boot stage
;
;  Booting from a CD needs no disk driver at all. The boot catalog asks the
;  BIOS to load the whole boot image (this sector followed by the kernel) at
;  load segment 0x0FE0, which puts this code at 0x0FE00 and the kernel at
;  0x10000 - exactly where the hard disk bootloader would have read it to.
;  So all that is left here is the memory map, A20, and protected mode.
;
;  Entry state: CS:IP = 0x0FE0:0x0000, DL = the emulated boot drive.
; =============================================================================

[BITS 16]
[ORG 0xFE00]

KERNEL_ADDR     equ 0x00010000      ; right after this 512 byte sector
BOOT_FLAG_ADDR  equ 0x00007000      ; where the kernel looks for the marker
BOOT_FLAG_CD    equ 0x4F534943      ; 'CISO' - booted from the install medium
E820_COUNT_ADDR equ 0x8000
E820_LIST_ADDR  equ 0x8004

CODE_SEG        equ 0x08
DATA_SEG        equ 0x10

entry:
    jmp     0x0000:start            ; absolute far jump normalises CS to 0

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti

    ; Tell the kernel it came off the install medium rather than a disk.
    mov     dword [BOOT_FLAG_ADDR], BOOT_FLAG_CD

    mov     si, msg_boot
    call    print

    call    detect_memory

    mov     si, msg_pmode
    call    print

    call    enable_a20

    cli
    lgdt    [gdt_descriptor]
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    jmp     CODE_SEG:protected_mode

; -----------------------------------------------------------------------------
print:
    pusha
    mov     ah, 0x0E
    mov     bx, 0x0007
.loop:
    lodsb
    test    al, al
    jz      .done
    int     0x10
    jmp     .loop
.done:
    popa
    ret

; -----------------------------------------------------------------------------
; detect_memory: INT 15h, EAX=E820 memory map into 0x8000
; -----------------------------------------------------------------------------
detect_memory:
    pusha
    mov     di, E820_LIST_ADDR
    xor     ebx, ebx
    xor     bp, bp
    mov     edx, 0x534D4150         ; 'SMAP'
    mov     eax, 0xE820
    mov     dword [es:di + 20], 1
    mov     ecx, 24
    int     0x15
    jc      .done
    cmp     eax, 0x534D4150
    jne     .done
    jmp     .store
.next:
    mov     eax, 0xE820
    mov     dword [es:di + 20], 1
    mov     ecx, 24
    int     0x15
    jc      .done
.store:
    jcxz    .skip
    mov     ecx, [es:di + 8]
    or      ecx, [es:di + 12]
    jz      .skip
    inc     bp
    add     di, 24
.skip:
    test    ebx, ebx
    jnz     .next
.done:
    mov     [E820_COUNT_ADDR], bp
    popa
    ret

; -----------------------------------------------------------------------------
enable_a20:
    in      al, 0x92
    test    al, 2
    jnz     .done
    or      al, 2
    and     al, 0xFE                ; bit 0 would trigger a reset
    out     0x92, al
.done:
    ret

; =============================================================================
[BITS 32]
protected_mode:
    mov     ax, DATA_SEG
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, 0x00090000
    jmp     CODE_SEG:KERNEL_ADDR

; =============================================================================
[BITS 16]

align 8
gdt_start:
    dq      0x0000000000000000
gdt_code:
    dw      0xFFFF, 0x0000
    db      0x00, 10011010b, 11001111b, 0x00
gdt_data:
    dw      0xFFFF, 0x0000
    db      0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_descriptor:
    dw      gdt_end - gdt_start - 1
    dd      gdt_start

msg_boot:   db "LightningOS install medium", 13, 10, 0
msg_pmode:  db "Entering protected mode", 13, 10, 0

times 512 - ($ - $$) db 0
