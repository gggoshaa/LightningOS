#!/usr/bin/env python3
"""Boots build/lightningos.img.

Prefers QEMU. If it is not installed, falls back to VirtualBox, which is
driven entirely from the command line: the raw image is converted to a VDI,
attached to a throwaway VM, and the VM is started.
"""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
IMAGE = os.path.join(BUILD, "lightningos.img")
VDI = os.path.join(BUILD, "lightningos.vdi")
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


def run_qemu(qemu, extra):
    command = [
        qemu,
        "-drive", "file=%s,format=raw,index=0,media=disk" % IMAGE,
        "-m", "128M",
        "-name", "LightningOS",
        "-rtc", "base=utc",
    ] + extra

    print("Booting LightningOS in QEMU...")
    subprocess.run(command)


def run_virtualbox(vbox, headless=False):
    print("QEMU not found - booting LightningOS in VirtualBox instead.")

    # Tear down anything left over from a previous run so the fresh image is
    # the one that actually boots.
    existing = vbox_run(vbox, ["list", "vms"], check=False).stdout
    if '"%s"' % VM_NAME in existing:
        vbox_run(vbox, ["controlvm", VM_NAME, "poweroff"], check=False)
        vbox_run(vbox, ["unregistervm", VM_NAME, "--delete"], check=False)
    if os.path.isfile(VDI):
        vbox_run(vbox, ["closemedium", "disk", VDI, "--delete"], check=False)
        if os.path.isfile(VDI):
            os.remove(VDI)

    print("Converting the raw image to VDI...")
    vbox_run(vbox, ["convertfromraw", IMAGE, VDI, "--format", "VDI"])

    os.makedirs(VM_FOLDER, exist_ok=True)
    vbox_run(vbox, ["createvm", "--name", VM_NAME, "--ostype", "Other",
                    "--basefolder", VM_FOLDER, "--register"])
    vbox_run(vbox, ["modifyvm", VM_NAME,
                    "--memory", "128",
                    "--boot1", "disk",
                    "--boot2", "none",
                    "--nic1", "none",
                    "--audio", "none",
                    "--usb", "off",
                    "--graphicscontroller", "vboxvga",
                    "--firmware", "bios"])
    vbox_run(vbox, ["storagectl", VM_NAME, "--name", "IDE",
                    "--add", "ide", "--controller", "PIIX4"])
    vbox_run(vbox, ["storageattach", VM_NAME, "--storagectl", "IDE",
                    "--port", "0", "--device", "0",
                    "--type", "hdd", "--medium", VDI])

    print("Starting the virtual machine...")
    vbox_run(vbox, ["startvm", VM_NAME,
                    "--type", "headless" if headless else "gui"])
    if headless:
        print("Running headless. Grab the screen with:")
        print("  VBoxManage controlvm %s screenshotpng shot.png" % VM_NAME)
        print("  VBoxManage controlvm %s poweroff" % VM_NAME)
    else:
        print("The VM window is open. Close it (or run 'VBoxManage controlvm "
              "%s poweroff') to stop." % VM_NAME)


def main():
    if not os.path.isfile(IMAGE):
        print("No disk image yet - run 'python build.py' first.", file=sys.stderr)
        sys.exit(1)

    extra = [a for a in sys.argv[1:] if a != "--headless"]
    headless = "--headless" in sys.argv[1:]

    qemu = find(["qemu-system-i386", "qemu-system-x86_64"], QEMU_DIRS)
    if qemu:
        run_qemu(qemu, extra + (["-display", "none"] if headless else []))
        return

    vbox = find(["VBoxManage"], VBOX_DIRS)
    if vbox:
        run_virtualbox(vbox, headless)
        return

    print("No emulator found. Install one of:", file=sys.stderr)
    print("  winget install SoftwareFreedomConservancy.QEMU   (~1.5 GB)",
          file=sys.stderr)
    print("  winget install Oracle.VirtualBox", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    main()
