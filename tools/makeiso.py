#!/usr/bin/env python3
"""A very small ISO 9660 writer with an El Torito boot record.

Enough of the standard to produce a disc that a PC BIOS will boot and that a
host operating system will still mount and read. Deliberately minimal: one
directory (the root), a handful of files in it, and a no-emulation boot entry.

Layout, in 2048 byte sectors:

    16   primary volume descriptor
    17   boot record volume descriptor (points at the boot catalog)
    18   volume descriptor set terminator
    19   boot catalog
    20   little endian path table
    21   big endian path table
    22   root directory
    23+  file data, boot image last so its sector number is easy to pin down
"""

import struct

SECTOR = 2048


def pad(data, size):
    if len(data) > size:
        raise ValueError("%d bytes do not fit in %d" % (len(data), size))
    return data + b"\0" * (size - len(data))


def sectors_for(length):
    return (length + SECTOR - 1) // SECTOR


def both_endian16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def both_endian32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def dec_datetime(when):
    """The 17 byte form used in the volume descriptors."""
    text = "%04d%02d%02d%02d%02d%02d00" % (when[0], when[1], when[2],
                                           when[3], when[4], when[5])
    return text.encode("ascii") + b"\0"


def dir_datetime(when):
    """The 7 byte form used in directory records."""
    return struct.pack("BBBBBBb", when[0] - 1900, when[1], when[2],
                       when[3], when[4], when[5], 0)


def directory_record(name, extent, length, when, is_dir, special=None):
    """One directory record. `special` is 0 for "." and 1 for ".."."""
    if special is None:
        identifier = name.encode("ascii")
    else:
        identifier = bytes([special])

    record = bytearray()
    record.append(0)                            # length, filled in below
    record.append(0)                            # extended attribute length
    record += both_endian32(extent)
    record += both_endian32(length)
    record += dir_datetime(when)
    record.append(0x02 if is_dir else 0x00)     # flags
    record.append(0)                            # file unit size
    record.append(0)                            # interleave gap
    record += both_endian16(1)                  # volume sequence number
    record.append(len(identifier))
    record += identifier
    if len(record) % 2:
        record.append(0)                        # pad to an even length
    record[0] = len(record)
    return bytes(record)


def primary_volume_descriptor(volume_id, total_sectors, path_table_size,
                              le_path_table, be_path_table, root_record, when):
    pvd = bytearray()
    pvd.append(1)                               # type: primary
    pvd += b"CD001"
    pvd.append(1)                               # version
    pvd.append(0)                               # unused
    pvd += pad(b"LIGHTNINGOS", 32)              # system identifier
    pvd += pad(volume_id.encode("ascii"), 32)
    pvd += b"\0" * 8
    pvd += both_endian32(total_sectors)
    pvd += b"\0" * 32
    pvd += both_endian16(1)                     # volume set size
    pvd += both_endian16(1)                     # volume sequence number
    pvd += both_endian16(SECTOR)
    pvd += both_endian32(path_table_size)
    pvd += struct.pack("<I", le_path_table)     # type L path table
    pvd += struct.pack("<I", 0)                 # optional type L
    pvd += struct.pack(">I", be_path_table)     # type M path table
    pvd += struct.pack(">I", 0)                 # optional type M
    pvd += root_record                          # exactly 34 bytes
    pvd += pad(b"", 128)                        # volume set identifier
    pvd += pad(b"LIGHTNINGOS", 128)             # publisher
    pvd += pad(b"", 128)                        # data preparer
    pvd += pad(b"MAKEISO.PY", 128)              # application
    pvd += pad(b"", 37) * 3                     # copyright, abstract, biblio
    pvd += dec_datetime(when)                   # creation
    pvd += dec_datetime(when)                   # modification
    pvd += b"\0" * 17                           # expiration
    pvd += b"\0" * 17                           # effective
    pvd.append(1)                               # file structure version
    return pad(bytes(pvd), SECTOR)


def boot_record_descriptor(catalog_sector):
    brvd = bytearray()
    brvd.append(0)                              # type: boot record
    brvd += b"CD001"
    brvd.append(1)                              # version
    brvd += pad(b"EL TORITO SPECIFICATION", 32)
    brvd += b"\0" * 32                          # unused
    brvd += struct.pack("<I", catalog_sector)
    return pad(bytes(brvd), SECTOR)


def boot_catalog(image_sector, load_segment, sector_count):
    """Validation entry plus one default entry, no emulation."""
    validation = bytearray()
    validation.append(1)                        # header id
    validation.append(0)                        # platform: 80x86
    validation += b"\0\0"                       # reserved
    validation += pad(b"LIGHTNINGOS", 24)       # manufacturer
    validation += b"\0\0"                       # checksum, filled in below
    validation += b"\x55\xAA"

    # The 16-bit words of the validation entry must sum to zero.
    words = struct.unpack("<16H", bytes(validation))
    checksum = (-sum(words)) & 0xFFFF
    validation[28:30] = struct.pack("<H", checksum)

    entry = bytearray()
    entry.append(0x88)                          # bootable
    entry.append(0x00)                          # no emulation
    entry += struct.pack("<H", load_segment)
    entry.append(0)                             # system type
    entry.append(0)                             # unused
    entry += struct.pack("<H", sector_count)    # 512 byte sectors to load
    entry += struct.pack("<I", image_sector)
    entry += b"\0" * 20

    return pad(bytes(validation) + bytes(entry), SECTOR)


def build(path, volume_id, boot_image, files, load_segment, boot_load_sectors,
          when=(2026, 1, 1, 0, 0, 0)):
    """Writes the ISO. `files` is a list of (NAME.EXT, bytes) for the root."""

    pvd_sector = 16
    brvd_sector = 17
    terminator_sector = 18
    catalog_sector = 19
    le_path_sector = 20
    be_path_sector = 21
    root_sector = 22
    data_sector = 23

    entries = list(files) + [("BOOT.IMG", boot_image)]

    # Lay the file data out, boot image last.
    placed = []
    cursor = data_sector
    for name, content in entries:
        placed.append((name, cursor, len(content), content))
        cursor += sectors_for(len(content))
    total_sectors = cursor

    boot_image_sector = placed[-1][1]

    # The root directory: ".", "..", then one record per file.
    root = bytearray()
    root += directory_record("", root_sector, SECTOR, when, True, special=0)
    root += directory_record("", root_sector, SECTOR, when, True, special=1)
    for name, extent, length, _ in placed:
        root += directory_record(name + ";1", extent, length, when, False)
    if len(root) > SECTOR:
        raise ValueError("the root directory needs more than one sector")
    root_data = pad(bytes(root), SECTOR)

    # Path table: just the root.
    le_path = bytearray()
    le_path.append(1)                           # identifier length
    le_path.append(0)                           # extended attribute length
    le_path += struct.pack("<I", root_sector)
    le_path += struct.pack("<H", 1)             # parent
    le_path.append(0)                           # identifier for the root
    le_path.append(0)                           # padding
    path_table_size = len(le_path)

    be_path = bytearray()
    be_path.append(1)
    be_path.append(0)
    be_path += struct.pack(">I", root_sector)
    be_path += struct.pack(">H", 1)
    be_path.append(0)
    be_path.append(0)

    # The copy of the root record that lives inside the PVD is fixed at 34
    # bytes, which is what a record with a one byte identifier comes to.
    root_record = directory_record("", root_sector, SECTOR, when, True,
                                   special=0)
    if len(root_record) != 34:
        raise ValueError("root record is %d bytes, expected 34"
                         % len(root_record))

    image = bytearray(b"\0" * (pvd_sector * SECTOR))
    image += primary_volume_descriptor(volume_id, total_sectors,
                                       path_table_size, le_path_sector,
                                       be_path_sector, root_record, when)
    image += boot_record_descriptor(catalog_sector)

    terminator = bytearray()
    terminator.append(0xFF)
    terminator += b"CD001"
    terminator.append(1)
    image += pad(bytes(terminator), SECTOR)

    image += boot_catalog(boot_image_sector, load_segment, boot_load_sectors)
    image += pad(bytes(le_path), SECTOR)
    image += pad(bytes(be_path), SECTOR)
    image += root_data

    for _, _, _, content in placed:
        image += pad(content, sectors_for(len(content)) * SECTOR)

    with open(path, "wb") as f:
        f.write(bytes(image))

    return {
        "sectors": total_sectors,
        "bytes": len(image),
        "boot_image_sector": boot_image_sector,
        "boot_load_sectors": boot_load_sectors,
    }
