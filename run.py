#!/usr/bin/env python3
"""Boots LightningOS, either the built system or the install medium.

Two modes:

  python run.py            the ready made system
      disk 0  build/lightningos.img  bootloader + kernel, rebuilt every time
      disk 1  build/data.img         persistent data, created once

  python run.py --iso      the installer, the way a user would meet it
      cd      build/lightningos.iso  boots into the installer
      disk 0  build/target.img       blank disk for it to install onto

  python run.py --installed   the disk the installer just wrote, on its own
      disk 0  build/target.img       one disk holding kernel and data

Keeping the boot disk and the data disk apart in the first mode is the point:
rebuilding the kernel replaces disk 0 and leaves saved data alone.

Prefers QEMU; falls back to VirtualBox, driven entirely from the command line.
"""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")

BOOT_IMG = os.path.join(BUILD, "lightningos.img")
DATA_IMG = os.path.join(BUILD, "data.img")
ISO = os.path.join(BUILD, "lightningos.iso")
TARGET_IMG = os.path.join(BUILD, "target.img")

BOOT_VDI = os.path.join(BUILD, "lightningos.vdi")
DATA_VDI = os.path.join(BUILD, "data.vdi")
TARGET_VDI = os.path.join(BUILD, "target.vdi")

DATA_MB = 8
TARGET_MB = 32
VM_NAME = "LightningOS"
VM_FOLDER = os.path.join(BUILD, "vm")

QEMU_DIRS = [
    r"C:\Program Files\qemu",
    r"C:\Program Files (x86)\qemu",
    os.path.expandvars(r"%LOCALAPPDATA%\Programs\qemu"),
]
VBOX_DIRS = [
    r"C:\Program Files\Oracle\VirtualBox",
    r"C:\Program Files (x86)\Oracle\VirtualBox",
]


def find(names, directories):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    for directory in directories:
        for name in names:
            candidate = os.path.join(directory, name + ".exe")
            if os.path.isfile(candidate):
                return candidate
    return None


def vbox_run(vbox, args, check=True):
    result = subprocess.run([vbox] + args, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    if check and result.returncode != 0:
        print(" ".join(["VBoxManage"] + args), file=sys.stderr)
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        sys.exit(result.returncode)
    return result


def ensure_raw(path, megabytes):
    """Creates a blank raw disk once. Existing contents are never touched."""
    if os.path.isfile(path):
        return False
    with open(path, "wb") as f:
        f.truncate(megabytes * 1024 * 1024)
    print("Created a blank %d MiB disk at %s" % (megabytes, path))
    return True


def drop_vdi(vbox, path, device):
    """Detaches and unregisters a VDI so its file can be deleted.

    Removing the file behind VirtualBox's back leaves the UUID registered and
    the next createmedium then fails with a conflict.
    """
    if vbox and os.path.isfile(path):
        vbox_run(vbox, ["controlvm", VM_NAME, "poweroff"], check=False)
        vbox_run(vbox, ["storageattach", VM_NAME, "--storagectl", "IDE",
                        "--port", "0", "--device", str(device),
                        "--medium", "none"], check=False)
        vbox_run(vbox, ["closemedium", "disk", path, "--delete"], check=False)
    if os.path.isfile(path):
        os.remove(path)


def reset_data():
    vbox = find(["VBoxManage"], VBOX_DIRS)
    drop_vdi(vbox, DATA_VDI, 1)
    if os.path.isfile(DATA_IMG):
        os.remove(DATA_IMG)
    print("Data disk reset - the next boot will run first time setup.")


def reset_target():
    vbox = find(["VBoxManage"], VBOX_DIRS)
    drop_vdi(vbox, TARGET_VDI, 0)
    if os.path.isfile(TARGET_IMG):
        os.remove(TARGET_IMG)
    print("Install target reset - it is a blank disk again.")


def run_qemu(qemu, mode, extra):
    if mode == "installed":
        command = [
            qemu,
            "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % TARGET_IMG,
        ]
        print("Booting the installed LightningOS in QEMU...")
    elif mode == "iso":
        ensure_raw(TARGET_IMG, TARGET_MB)
        command = [
            qemu,
            "-cdrom", ISO,
            "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % TARGET_IMG,
            "-boot", "d",
        ]
        print("Booting the LightningOS install medium in QEMU...")
    else:
        ensure_raw(DATA_IMG, DATA_MB)
        command = [
            qemu,
            "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % BOOT_IMG,
            "-drive", "file=%s,format=raw,if=ide,index=1,media=disk" % DATA_IMG,
        ]
        print("Booting LightningOS in QEMU...")

    command += ["-m", "128M", "-name", "LightningOS", "-rtc", "base=utc"]
    command += extra
    subprocess.run(command)


def teardown_vm(vbox):
    existing = vbox_run(vbox, ["list", "vms"], check=False).stdout
    if '"%s"' % VM_NAME not in existing:
        return

    vbox_run(vbox, ["controlvm", VM_NAME, "poweroff"], check=False)
    for port, device in ((0, 0), (0, 1), (1, 0)):
        vbox_run(vbox, ["storageattach", VM_NAME, "--storagectl", "IDE",
                        "--port", str(port), "--device", str(device),
                        "--medium", "none"], check=False)
    vbox_run(vbox, ["unregistervm", VM_NAME, "--delete"], check=False)


def create_vm(vbox, boot_device):
    os.makedirs(VM_FOLDER, exist_ok=True)
    vbox_run(vbox, ["createvm", "--name", VM_NAME, "--ostype", "Other",
                    "--basefolder", VM_FOLDER, "--register"])
    vbox_run(vbox, ["modifyvm", VM_NAME,
                    "--memory", "128",
                    "--boot1", boot_device,
                    "--boot2", "disk" if boot_device == "dvd" else "none",
                    "--nic1", "none",
                    "--audio", "none",
                    "--usb", "off",
                    "--graphicscontroller", "vboxvga",
                    "--firmware", "bios"])
    # Host I/O caching is off on purpose. With it on, VirtualBox keeps guest
    # writes in host memory, and killing the VM with `controlvm poweroff`
    # throws them away - the guest believes the snapshot was written while the
    # VDI on disk never received it.
    vbox_run(vbox, ["storagectl", VM_NAME, "--name", "IDE",
                    "--add", "ide", "--controller", "PIIX4",
                    "--hostiocache", "off"])


def attach(vbox, port, device, kind, medium):
    vbox_run(vbox, ["storageattach", VM_NAME, "--storagectl", "IDE",
                    "--port", str(port), "--device", str(device),
                    "--type", kind, "--medium", medium])


def run_virtualbox(vbox, mode, headless):
    print("QEMU not found - using VirtualBox instead.")
    teardown_vm(vbox)

    if mode == "installed":
        if not os.path.isfile(TARGET_VDI):
            print("Nothing installed yet - run 'python run.py --iso' first.",
                  file=sys.stderr)
            sys.exit(1)
        vbox_run(vbox, ["closemedium", "disk", TARGET_VDI], check=False)
        create_vm(vbox, "disk")
        attach(vbox, 0, 0, "hdd", TARGET_VDI)
        print("Booting the installed LightningOS...")
    elif mode == "iso":
        # The target disk keeps whatever the installer wrote, so it is only
        # created when missing.
        if os.path.isfile(TARGET_VDI):
            vbox_run(vbox, ["closemedium", "disk", TARGET_VDI], check=False)
        else:
            print("Creating a blank %d MiB target disk..." % TARGET_MB)
            vbox_run(vbox, ["createmedium", "disk", "--filename", TARGET_VDI,
                            "--size", str(TARGET_MB), "--format", "VDI"])

        create_vm(vbox, "dvd")
        attach(vbox, 0, 0, "hdd", TARGET_VDI)
        attach(vbox, 1, 0, "dvddrive", ISO)
        print("Booting the LightningOS install medium...")
    else:
        # The boot disk is regenerated from the freshly built image each run.
        if os.path.isfile(BOOT_VDI):
            vbox_run(vbox, ["closemedium", "disk", BOOT_VDI, "--delete"],
                     check=False)
            if os.path.isfile(BOOT_VDI):
                os.remove(BOOT_VDI)

        print("Converting the boot image to VDI...")
        vbox_run(vbox, ["convertfromraw", BOOT_IMG, BOOT_VDI, "--format", "VDI"])

        if os.path.isfile(DATA_VDI):
            vbox_run(vbox, ["closemedium", "disk", DATA_VDI], check=False)
        else:
            print("Creating a blank %d MiB data disk..." % DATA_MB)
            vbox_run(vbox, ["createmedium", "disk", "--filename", DATA_VDI,
                            "--size", str(DATA_MB), "--format", "VDI"])

        create_vm(vbox, "disk")
        attach(vbox, 0, 0, "hdd", BOOT_VDI)
        attach(vbox, 0, 1, "hdd", DATA_VDI)
        print("Booting LightningOS...")

    vbox_run(vbox, ["startvm", VM_NAME,
                    "--type", "headless" if headless else "gui"])
    if headless:
        print("Running headless. Grab the screen with:")
        print("  VBoxManage controlvm %s screenshotpng shot.png" % VM_NAME)
        print("  VBoxManage controlvm %s poweroff" % VM_NAME)
    else:
        print("The VM window is open. Use 'shutdown' inside the OS so that "
              "the filesystem is written to disk before it stops.")


def main():
    args = sys.argv[1:]

    if "--reset-data" in args:
        reset_data()
        args = [a for a in args if a != "--reset-data"]
    if "--reset-target" in args:
        reset_target()
        args = [a for a in args if a != "--reset-target"]

    headless = "--headless" in args
    if "--iso" in args:
        mode = "iso"
    elif "--installed" in args:
        mode = "installed"
    else:
        mode = "built"
    extra = [a for a in args
             if a not in ("--iso", "--installed", "--headless")]

    wanted = {"iso": ISO, "installed": TARGET_IMG, "built": BOOT_IMG}[mode]
    if mode == "installed" and find(["VBoxManage"], VBOX_DIRS) and             not find(["qemu-system-i386", "qemu-system-x86_64"], QEMU_DIRS):
        wanted = TARGET_VDI
    if not os.path.isfile(wanted):
        if not args:
            return              # a bare reset, nothing to boot
        print("%s is missing - run 'python build.py' first." % wanted,
              file=sys.stderr)
        sys.exit(1)

    qemu = find(["qemu-system-i386", "qemu-system-x86_64"], QEMU_DIRS)
    if qemu:
        run_qemu(qemu, mode, extra + (["-display", "none"] if headless else []))
        return

    vbox = find(["VBoxManage"], VBOX_DIRS)
    if vbox:
        run_virtualbox(vbox, mode, headless)
        return

    print("No emulator found. Install one of:", file=sys.stderr)
    print("  winget install SoftwareFreedomConservancy.QEMU   (~1.5 GB)",
          file=sys.stderr)
    print("  winget install Oracle.VirtualBox", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    main()
