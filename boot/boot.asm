; =============================================================================
;  LightningOS - stage 1 bootloader (MBR, 512 bytes)
;
;  BIOS loads this at 0x7C00 in 16-bit real mode with DL = boot drive.
;  What it does:
;    1. sets up a stack and saves the boot drive
;    2. asks the BIOS for the memory map (INT 15h / E820) and stores it at 0x8000
;    3. loads the kernel image from LBA 1 to physical 0x10000
;    4. enables the A20 gate
;    5. loads a flat GDT, switches to 32-bit protected mode
;    6. jumps to the kernel entry point
; =============================================================================

[BITS 16]
[ORG 0x7C00]

KERNEL_SEG      equ 0x1000          ; kernel lands at 0x1000:0000 = 0x00010000
KERNEL_LBA      equ 1               ; kernel starts right after the boot sector
CHUNK_SECTORS   equ 64              ; sectors per BIOS call (64 * 512 = 32 KiB)
CHUNK_COUNT     equ 4               ; 4 * 32 KiB = 128 KiB of kernel space
E820_COUNT_ADDR equ 0x8000          ; word: number of memory map entries
E820_LIST_ADDR  equ 0x8004          ; array of 24-byte entries

CODE_SEG        equ 0x08
DATA_SEG        equ 0x10

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00              ; stack grows down from the bootloader
    sti

    mov     [boot_drive], dl

    ; Clear the marker the CD stage would have set, so a disk boot is never
    ; mistaken for a boot off the install medium.
    mov     dword [0x7000], 0

    mov     si, msg_load
    call    print

    call    detect_memory
    call    load_kernel

    mov     si, msg_pmode
    call    print

    call    enable_a20

    cli
    lgdt    [gdt_descriptor]
    mov     eax, cr0
    or      eax, 1                  ; CR0.PE - protected mode enable
    mov     cr0, eax
    jmp     CODE_SEG:protected_mode ; far jump flushes the prefetch queue

; -----------------------------------------------------------------------------
; print: writes the NUL terminated string at DS:SI using BIOS teletype output
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
    xor     bp, bp                  ; entry counter
    mov     edx, 0x534D4150         ; 'SMAP'
    mov     eax, 0xE820
    mov     dword [es:di + 20], 1   ; force a valid ACPI 3.0 extended attribute
    mov     ecx, 24
    int     0x15
    jc      .done                   ; carry on the first call => unsupported
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
    jcxz    .skip                   ; zero length entry -> ignore
    mov     ecx, [es:di + 8]        ; low dword of the region length
    or      ecx, [es:di + 12]       ; high dword
    jz      .skip
    inc     bp
    add     di, 24
.skip:
    test    ebx, ebx                ; ebx == 0 => that was the last entry
    jnz     .next
.done:
    mov     [E820_COUNT_ADDR], bp
    popa
    ret

; -----------------------------------------------------------------------------
; load_kernel: reads CHUNK_COUNT chunks of CHUNK_SECTORS sectors using the
;              BIOS LBA extension (INT 13h, AH=42h)
; -----------------------------------------------------------------------------
load_kernel:
    pusha
    mov     cx, CHUNK_COUNT
.chunk:
    push    cx
    mov     bl, 4                   ; retries per chunk
.try:
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jnc     .ok
    xor     ah, ah                  ; reset the disk controller and retry
    mov     dl, [boot_drive]
    int     0x13
    dec     bl
    jnz     .try
    jmp     disk_error
.ok:
    add     word [dap_seg], CHUNK_SECTORS * 512 / 16
    add     dword [dap_lba], CHUNK_SECTORS
    mov     al, '.'
    mov     ah, 0x0E
    int     0x10                    ; progress dot
    pop     cx
    loop    .chunk
    popa
    ret

disk_error:
    mov     si, msg_disk_err
    call    print
    cli
.hang:
    hlt
    jmp     .hang

; -----------------------------------------------------------------------------
; enable_a20: fast A20 through the system control port 0x92
; -----------------------------------------------------------------------------
enable_a20:
    in      al, 0x92
    test    al, 2
    jnz     .done
    or      al, 2
    and     al, 0xFE                ; never touch bit 0 - it triggers a reset
    out     0x92, al
.done:
    ret

; =============================================================================
;  32-bit protected mode
; =============================================================================
[BITS 32]
protected_mode:
    mov     ax, DATA_SEG
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, 0x00090000         ; temporary stack below the 640K boundary
    jmp     CODE_SEG:(KERNEL_SEG * 16)

; =============================================================================
;  data
; =============================================================================
[BITS 16]

; Disk Address Packet for INT 13h AH=42h
align 4
dap:
    db      0x10                    ; packet size
    db      0
    dw      CHUNK_SECTORS           ; sectors to transfer
dap_off:
    dw      0x0000                  ; destination offset
dap_seg:
    dw      KERNEL_SEG              ; destination segment
dap_lba:
    dd      KERNEL_LBA              ; starting LBA (low)
    dd      0                       ; starting LBA (high)

align 8
gdt_start:
    dq      0x0000000000000000      ; null descriptor
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

boot_drive:     db 0
msg_load:       db "LightningOS: loading kernel", 0
msg_pmode:      db 13, 10, "LightningOS: entering protected mode", 13, 10, 0
msg_disk_err:   db 13, 10, "FATAL: disk read error", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
