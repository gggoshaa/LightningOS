#!/usr/bin/env python3
"""Build script for LightningOS.

Produces build/lightningos.img - a raw disk image whose first sector is the
bootloader and whose following sectors hold the flat kernel binary.

Toolchain: nasm, clang (cross compiling to i386-elf), ld.lld, llvm-objcopy.
"""

import datetime
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "tools"))
import makeiso

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
OBJ = os.path.join(BUILD, "obj")

SECTOR = 512
KERNEL_LOAD_SECTORS = 256          # must match CHUNK_SECTORS*CHUNK_COUNT in boot.asm
IMAGE_SECTORS = 2880 * 4           # 5.7 MiB - comfortably larger than we read
CD_LOAD_SEGMENT = 0x0FE0           # puts the kernel at 0x10000, see cdboot.asm

CFLAGS = [
    "-target", "i386-elf",
    "-m32",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-pic",
    "-fno-pie",
    "-mno-sse",
    "-mno-sse2",
    "-mno-mmx",
    "-mno-red-zone",
    "-fno-asynchronous-unwind-tables",
    "-std=gnu11",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    # Stamped in here rather than read from __DATE__, which clang refuses as
    # non-reproducible when zig cc turns -Wdate-time into an error.
    "-DLOS_BUILD_STAMP=" + datetime.datetime.now().strftime("%Y-%m-%dT%H:%M"),
    "-I", os.path.join(ROOT, "include"),
]

# entry.asm has to be linked first so that _start lands at 0x10000.
ASM_SOURCES = [
    "kernel/entry.asm",
    "kernel/gdt_flush.asm",
    "kernel/interrupt.asm",
]

C_SOURCES = [
    "kernel/kernel.c",
    "kernel/gdt.c",
    "kernel/idt.c",
    "kernel/mem.c",
    "kernel/panic.c",
    "kernel/task.c",
    "kernel/console.c",
    "kernel/users.c",
    "kernel/shell.c",
    "kernel/banner.c",
    "kernel/install.c",
    "kernel/bootsector.c",
    "drivers/vga.c",
    "drivers/serial.c",
    "drivers/timer.c",
    "drivers/keyboard.c",
    "drivers/mouse.c",
    "drivers/rtc.c",
    "drivers/ata.c",
    "fs/ramfs.c",
    "fs/persist.c",
    "lib/string.c",
    "lib/printf.c",
]


def find_tool(name, extra_dirs=()):
    """Locates an executable on PATH or in a few well known install folders."""
    found = shutil.which(name)
    if found:
        return found
    for directory in extra_dirs:
        candidate = os.path.join(directory, name + ".exe")
        if os.path.isfile(candidate):
            return candidate
    return None


LLVM_DIRS = [
    r"C:\Program Files\LLVM\bin",
    r"C:\Program Files (x86)\LLVM\bin",
    os.path.expandvars(r"%LOCALAPPDATA%\Programs\LLVM\bin"),
]
NASM_DIRS = [
    r"C:\Program Files\NASM",
    r"C:\Program Files (x86)\NASM",
    os.path.expandvars(r"%LOCALAPPDATA%\bin\NASM"),
]
ZIG_DIRS = [
    os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Links"),
    os.path.expandvars(r"%LOCALAPPDATA%\Programs\zig"),
    r"C:\zig",
]


def glob_zig():
    """winget unpacks the zig zip into a versioned folder, so look around."""
    roots = [os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages")]
    for root in roots:
        if not os.path.isdir(root):
            continue
        for entry in os.listdir(root):
            if "zig" not in entry.lower():
                continue
            base = os.path.join(root, entry)
            for current, _dirs, files in os.walk(base):
                if "zig.exe" in files:
                    return os.path.join(current, "zig.exe")
    return None


class Toolchain:
    """Two supported backends, both producing a 32-bit ELF kernel.

    llvm: clang + ld.lld + llvm-objcopy from a full LLVM install
    zig:  the same components wrapped in the (much smaller) zig toolchain
    """

    def __init__(self, nasm, kind, clang, linker, objcopy):
        self.nasm = nasm
        self.kind = kind
        self.clang = clang
        self.linker = linker
        self.objcopy = objcopy

    def compile_cmd(self, source, out):
        if self.kind == "llvm":
            return [self.clang] + CFLAGS + ["-c", source, "-o", out]
        return ([self.clang, "cc", "-target", "x86-freestanding"]
                + [f for f in CFLAGS if f not in ("-target", "i386-elf")]
                + ["-c", source, "-o", out])

    def link_cmd(self, script, out, objects):
        if self.kind == "llvm":
            return ([self.linker, "-m", "elf_i386", "-nostdlib",
                     "--build-id=none", "-T", script, "-o", out] + objects)
        return ([self.clang, "cc", "-target", "x86-freestanding", "-nostdlib",
                 "-static", "-Wl,--build-id=none", "-Wl,-T," + script,
                 "-o", out] + objects)

    def objcopy_cmd(self, elf, binary):
        if self.kind == "llvm":
            return [self.objcopy, "-O", "binary", elf, binary]
        return [self.clang, "objcopy", "-O", "binary", elf, binary]


def tools():
    nasm = find_tool("nasm", NASM_DIRS)
    clang = find_tool("clang", LLVM_DIRS)
    linker = find_tool("ld.lld", LLVM_DIRS)
    objcopy = find_tool("llvm-objcopy", LLVM_DIRS)
    zig = find_tool("zig", ZIG_DIRS) or glob_zig()

    if not nasm:
        print("nasm not found - install it with: winget install NASM.NASM",
              file=sys.stderr)
        sys.exit(1)

    if clang and linker and objcopy:
        return Toolchain(nasm, "llvm", clang, linker, objcopy)
    if zig:
        return Toolchain(nasm, "zig", zig, zig, zig)

    print("No C toolchain found. Install either:", file=sys.stderr)
    print("  winget install zig.zig        (small, portable, no admin)",
          file=sys.stderr)
    print("  winget install LLVM.LLVM      (large, needs ~4 GB free)",
          file=sys.stderr)
    sys.exit(1)


def run(command):
    print("  " + " ".join(os.path.basename(command[0]).split()) + " " +
          " ".join(command[1:][-3:]))
    result = subprocess.run(command, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    if result.stdout.strip():
        print(result.stdout.rstrip())
    if result.stderr.strip():
        print(result.stderr.rstrip())
    if result.returncode != 0:
        print("FAILED: " + " ".join(command), file=sys.stderr)
        sys.exit(result.returncode)


def generate_boot_sector_source(boot_bin):
    """Bakes the hard disk boot sector into the kernel.

    The installer cannot copy the boot sector from the running system: when
    booted from CD the first sector in memory is the El Torito stage, not the
    one a hard disk needs. So the real thing travels along as data.
    """
    with open(boot_bin, "rb") as f:
        data = f.read()

    lines = [
        "/* Generated by build.py from boot/boot.asm - do not edit. */",
        "",
        '#include "types.h"',
        "",
        "const uint8_t los_boot_sector[512] = {",
    ]
    for offset in range(0, len(data), 12):
        chunk = data[offset:offset + 12]
        lines.append("    " + " ".join("0x%02X," % b for b in chunk))
    lines.append("};")

    path = os.path.join(ROOT, "kernel", "bootsector.c")
    with open(path, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")


def object_path(source):
    name = source.replace("/", "_").rsplit(".", 1)[0] + ".o"
    return os.path.join(OBJ, name)


def main():
    tool = tools()

    os.makedirs(OBJ, exist_ok=True)
    print("toolchain: %s (%s)" % (tool.kind, tool.clang))

    print("== bootloader ==")
    boot_bin = os.path.join(BUILD, "boot.bin")
    run([tool.nasm, "-f", "bin",
         os.path.join(ROOT, "boot", "boot.asm"), "-o", boot_bin])

    size = os.path.getsize(boot_bin)
    if size != SECTOR:
        print("boot sector is %d bytes, expected %d" % (size, SECTOR),
              file=sys.stderr)
        sys.exit(1)

    cdboot_bin = os.path.join(BUILD, "cdboot.bin")
    run([tool.nasm, "-f", "bin",
         os.path.join(ROOT, "boot", "cdboot.asm"), "-o", cdboot_bin])

    size = os.path.getsize(cdboot_bin)
    if size != SECTOR:
        print("cd boot sector is %d bytes, expected %d" % (size, SECTOR),
              file=sys.stderr)
        sys.exit(1)

    generate_boot_sector_source(boot_bin)

    print("== assembling ==")
    objects = []
    for source in ASM_SOURCES:
        out = object_path(source)
        run([tool.nasm, "-f", "elf32", os.path.join(ROOT, source), "-o", out])
        objects.append(out)

    print("== compiling ==")
    for source in C_SOURCES:
        out = object_path(source)
        run(tool.compile_cmd(os.path.join(ROOT, source), out))
        objects.append(out)

    print("== linking ==")
    elf = os.path.join(BUILD, "kernel.elf")
    run(tool.link_cmd(os.path.join(ROOT, "linker.ld"), elf, objects))

    kernel_bin = os.path.join(BUILD, "kernel.bin")
    run(tool.objcopy_cmd(elf, kernel_bin))

    kernel_size = os.path.getsize(kernel_bin)
    max_size = KERNEL_LOAD_SECTORS * SECTOR
    if kernel_size > max_size:
        print("kernel is %d bytes, the bootloader only loads %d"
              % (kernel_size, max_size), file=sys.stderr)
        sys.exit(1)

    print("== disk image ==")
    image = os.path.join(BUILD, "lightningos.img")
    with open(boot_bin, "rb") as f:
        boot = f.read()
    with open(kernel_bin, "rb") as f:
        kernel = f.read()

    data = bytearray(IMAGE_SECTORS * SECTOR)
    data[0:len(boot)] = boot
    data[SECTOR:SECTOR + len(kernel)] = kernel
    with open(image, "wb") as f:
        f.write(data)

    print("== install medium ==")
    with open(cdboot_bin, "rb") as f:
        cdboot = f.read()

    # The El Torito boot image is the CD stage followed by the kernel. The
    # BIOS loads the whole thing at CD_LOAD_SEGMENT, which lands the kernel at
    # 0x10000 with no disk driver involved.
    boot_image = cdboot + kernel
    boot_load_sectors = (len(boot_image) + SECTOR - 1) // SECTOR

    readme = (
        "LightningOS %s install medium.\r\n"
        "\r\n"
        "Boot this disc to reach the installer. It can write the system onto\r\n"
        "a hard disk, or run it live without touching anything.\r\n"
        "\r\n"
        "Source: https://github.com/gggoshaa/LightningOS\r\n" % "2.0"
    ).encode("ascii")

    iso = os.path.join(BUILD, "lightningos.iso")
    info = makeiso.build(iso, "LIGHTNINGOS", boot_image,
                         [("README.TXT", readme)],
                         CD_LOAD_SEGMENT, boot_load_sectors)

    print()
    print("boot sector : %d bytes" % len(boot))
    print("kernel      : %d bytes (%d sectors, limit %d)"
          % (kernel_size, (kernel_size + SECTOR - 1) // SECTOR, KERNEL_LOAD_SECTORS))
    print("hard disk   : %s (%d KiB)" % (image, len(data) // 1024))
    print("iso         : %s (%d KiB)" % (iso, info["bytes"] // 1024))
    print("              boot image at CD sector %d, %d x 512 loaded at 0x%X"
          % (info["boot_image_sector"], info["boot_load_sectors"],
             CD_LOAD_SEGMENT * 16))
    print()
    print("Run the installed system:  python run.py")
    print("Boot the install medium :  python run.py --iso")


if __name__ == "__main__":
    main()
