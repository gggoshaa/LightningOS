#!/usr/bin/env python3
"""Boots LightningOS with two disks.

  disk 0 (primary master)  build/lightningos.img  - bootloader + kernel,
                                                    rebuilt by build.py
  disk 1 (primary slave)   build/data.img         - persistent data, created
                                                    once and never rewritten

Keeping the two apart is the point: rebuilding the kernel replaces disk 0 and
leaves everything the running system saved on disk 1 untouched.

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
BOOT_VDI = os.path.join(BUILD, "lightningos.vdi")
DATA_VDI = os.path.join(BUILD, "data.vdi")

DATA_MB = 8
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


def ensure_data_img():
    """Creates the raw data disk once. Existing data is never touched."""
    if os.path.isfile(DATA_IMG):
        return False
    with open(DATA_IMG, "wb") as f:
        f.truncate(DATA_MB * 1024 * 1024)
    print("Created a blank %d MiB data disk at %s" % (DATA_MB, DATA_IMG))
    return True


def reset_data():
    """Throws the data disk away so the next boot starts from scratch.

    The VDI has to be dropped from VirtualBox's media registry first: deleting
    the file behind its back leaves the UUID registered, and the next
    createmedium then fails with a UUID conflict.
    """
    vbox = find(["VBoxManage"], VBOX_DIRS)
    if vbox and os.path.isfile(DATA_VDI):
        vbox_run(vbox, ["controlvm", VM_NAME, "poweroff"], check=False)
        vbox_run(vbox, ["storageattach", VM_NAME, "--storagectl", "IDE",
                        "--port", "0", "--device", "1",
                        "--medium", "none"], check=False)
        vbox_run(vbox, ["closemedium", "disk", DATA_VDI, "--delete"], check=False)

    for path in (DATA_IMG, DATA_VDI):
        if os.path.isfile(path):
            os.remove(path)
    print("Data disk reset - the next boot will run first time setup.")


def run_qemu(qemu, extra):
    ensure_data_img()
    command = [
        qemu,
        "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % BOOT_IMG,
        "-drive", "file=%s,format=raw,if=ide,index=1,media=disk" % DATA_IMG,
        "-m", "128M",
        "-name", "LightningOS",
        "-rtc", "base=utc",
    ] + extra

    print("Booting LightningOS in QEMU...")
    subprocess.run(command)


def run_virtualbox(vbox, headless=False):
    print("QEMU not found - booting LightningOS in VirtualBox instead.")

    # Tear down the previous VM, detaching media first so that --delete can
    # only ever remove the machine's own files.
    existing = vbox_run(vbox, ["list", "vms"], check=False).stdout
    if '"%s"' % VM_NAME in existing:
        vbox_run(vbox, ["controlvm", VM_NAME, "poweroff"], check=False)
        for device in ("0", "1"):
            vbox_run(vbox, ["storageattach", VM_NAME, "--storagectl", "IDE",
                            "--port", "0", "--device", device,
                            "--medium", "none"], check=False)
        vbox_run(vbox, ["unregistervm", VM_NAME, "--delete"], check=False)

    # The boot disk is regenerated from the freshly built image every run.
    if os.path.isfile(BOOT_VDI):
        vbox_run(vbox, ["closemedium", "disk", BOOT_VDI, "--delete"], check=False)
        if os.path.isfile(BOOT_VDI):
            os.remove(BOOT_VDI)

    print("Converting the boot image to VDI...")
    vbox_run(vbox, ["convertfromraw", BOOT_IMG, BOOT_VDI, "--format", "VDI"])

    # The data disk is created once and then left alone forever. closemedium
    # without --delete only drops it from VirtualBox's registry, keeping the
    # file and everything the OS wrote into it.
    if os.path.isfile(DATA_VDI):
        vbox_run(vbox, ["closemedium", "disk", DATA_VDI], check=False)
    else:
        print("Creating a blank %d MiB data disk..." % DATA_MB)
        vbox_run(vbox, ["createmedium", "disk", "--filename", DATA_VDI,
                        "--size", str(DATA_MB), "--format", "VDI"])

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
    # Host I/O caching is off on purpose. With it on, VirtualBox keeps guest
    # writes in host memory, and killing the VM with `controlvm poweroff`
    # throws them away - the guest believes the snapshot was written while the
    # VDI on disk never received it.
    vbox_run(vbox, ["storagectl", VM_NAME, "--name", "IDE",
                    "--add", "ide", "--controller", "PIIX4",
                    "--hostiocache", "off"])
    vbox_run(vbox, ["storageattach", VM_NAME, "--storagectl", "IDE",
                    "--port", "0", "--device", "0",
                    "--type", "hdd", "--medium", BOOT_VDI])
    vbox_run(vbox, ["storageattach", VM_NAME, "--storagectl", "IDE",
                    "--port", "0", "--device", "1",
                    "--type", "hdd", "--medium", DATA_VDI])

    print("Starting the virtual machine...")
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
        if not args and not os.path.isfile(BOOT_IMG):
            return

    if not os.path.isfile(BOOT_IMG):
        print("No disk image yet - run 'python build.py' first.", file=sys.stderr)
        sys.exit(1)

    headless = "--headless" in args
    extra = [a for a in args if a != "--headless"]

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
