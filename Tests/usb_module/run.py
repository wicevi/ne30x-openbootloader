#!/usr/bin/env python3
"""Host-side test of the OpenBootloader USB transport, run step.

Executes build/test.elf (see build.sh) in the Unicorn CPU emulator and prints the
outcome. The firmware starts at its reset handler and, once run_tests() has completed,
reaches test_finished(), which reads an address outside the emulated memory: that access
is what stops the emulator. The results are then read from the firmware memory through
the ELF symbol table.

Exit code: 0 when every check passed, 1 otherwise, 2 when the run did not complete.
"""
import os
import struct
import subprocess
import sys

try:
    import unicorn
except ImportError:
    sys.exit("the 'unicorn' package is required: python -m pip install unicorn")

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_FETCH_UNMAPPED)
from unicorn.arm_const import UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_SP, UC_ARM_REG_PC

HERE = os.path.dirname(os.path.abspath(__file__))
ELF = os.path.join(HERE, "build", "test.elf")   # --elf <path> for another build

# Emulated memory map: the addresses are the ones of the target for the RAM the module writes
CODE_BASE, CODE_SIZE = 0x00010000, 2 * 1024 * 1024
DATA_BASE, DATA_SIZE = 0x20000000, 16 * 1024 * 1024
STACK_TOP = (DATA_BASE + DATA_SIZE - 8) & ~7      # inside the region: a push writes below SP
HEAP_BASE, HEAP_SIZE = 0x10000000, 0x00800000     # _sbrk area: the test images
# openbootloader_conf.h: where the external memory loader is downloaded, and the RAM download area
LOADER_BASE, LOADER_SIZE = 0x38000000, 32 * 1024
RAM_BASE, RAM_SIZE = 0x341B9000, 0x47000
TEST_DONE_ADDRESS = 0xE0000000                    # read by test_finished() to end the run


def symbols():
    out = subprocess.run(["arm-none-eabi-nm", ELF], capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"cannot read {ELF}: run build.sh first ({out.stderr.strip()})")
    return {parts[2]: int(parts[0], 16) for parts in (line.split() for line in out.stdout.splitlines())
            if len(parts) == 3}


def load_elf(uc):
    """Load the PT_LOAD segments at their physical load addresses; returns the entry point.

    The file image goes where a loader would put it (p_paddr: the .data initialisers live
    next to the code). The start-up code then copies .data to its virtual address and
    zeroes .bss, so nothing else has to be written here.
    """
    elf = open(ELF, "rb").read()
    assert elf[:4] == b"\x7fELF" and elf[4] == 1 and elf[5] == 1, "ELF32 little-endian expected"
    e_entry = struct.unpack_from("<I", elf, 0x18)[0]
    e_phoff = struct.unpack_from("<I", elf, 0x1C)[0]
    e_phnum = struct.unpack_from("<H", elf, 0x2C)[0]
    for i in range(e_phnum):
        p_type, p_offset, _p_vaddr, p_paddr, p_filesz, _p_memsz = struct.unpack_from("<8I", elf, e_phoff + i * 32)[:6]
        if p_type == 1:
            uc.mem_write(p_paddr, elf[p_offset:p_offset + p_filesz])
    return e_entry


# A flash layout directory built by a host tool (NE301: Script/usb_flash.py) can be handed
# to the device tests: --layout <dir>. It is stored in the emulated memory as a small
# directory followed by the files, and test T11 of the driver plays it.
#   LAYOUT_BASE: magic, file count, then per file {offset from LAYOUT_BASE, length}
#   file 0 is FlashLayout.tsv, the others the binaries it refers to, in its order
LAYOUT_BASE, LAYOUT_SIZE = 0x30000000, 0x01000000     # the complete NE301 download is 9.4 MB
LAYOUT_MAGIC = 0x4C41594F          # "OYAL"


def load_layout(uc, directory):
    """Store the layout directory in the emulator; returns the number of files."""
    directory = os.path.abspath(directory)
    tsv = os.path.join(directory, "FlashLayout.tsv")
    if not os.path.isfile(tsv):
        sys.exit(f"{tsv} not found in {directory}")
    names = []
    for line in open(tsv, encoding="ascii").read().split("\n"):
        cols = line.split("\t")
        if len(cols) == 7 and cols[0] == "P":
            names.append(cols[6])
    files = [("FlashLayout.tsv", open(tsv, "rb").read())]
    for name in names:
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            sys.exit(f"the layout refers to {name}, not found in {directory}")
        files.append((name, open(path, "rb").read()))

    header = struct.pack("<II", LAYOUT_MAGIC, len(files))
    offset = 8 + 8 * len(files)
    body = bytearray()
    for _, data in files:
        header += struct.pack("<II", offset + len(body), len(data))
        body += data
    if (8 + 8 * len(files) + len(body)) > LAYOUT_SIZE:
        sys.exit(f"the layout directory is too large for the emulator ({len(body)} bytes)")
    uc.mem_write(LAYOUT_BASE, header)
    uc.mem_write(LAYOUT_BASE + offset, bytes(body))
    print(f"external flash layout: {directory} ({len(files) - 1} binaries, {len(body)} bytes)")
    return len(files)


def main():
    import argparse
    ap = argparse.ArgumentParser(description="run the USB module tests in the Unicorn emulator")
    ap.add_argument("--layout", help="flash layout directory to hand to test T11 (see load_layout)")
    ap.add_argument("--elf", help="the test firmware to run (default: build/test.elf)")
    args = ap.parse_args()
    global ELF
    if args.elf:
        ELF = os.path.abspath(args.elf)

    if not os.path.isfile(ELF):
        sys.exit(f"{ELF} not found: run build.sh first")
    syms = symbols()

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)   # Unicorn has no M55: same ARMv8-M mainline ISA
    uc.mem_map(CODE_BASE, CODE_SIZE)
    uc.mem_map(DATA_BASE, DATA_SIZE)
    uc.mem_map(HEAP_BASE, HEAP_SIZE)
    uc.mem_map(LOADER_BASE, LOADER_SIZE)          # where the external memory loader is written
    uc.mem_map(RAM_BASE, RAM_SIZE)
    entry = load_elf(uc)
    # always mapped: the driver reads the magic to know whether a layout was given
    uc.mem_map(LAYOUT_BASE, LAYOUT_SIZE)
    if args.layout:
        load_layout(uc, args.layout)

    finished = [False]
    abort_info = [0, 0, 0]

    def on_unmapped(uc_, access, address, size, _value, _user):
        if address == TEST_DONE_ADDRESS:
            finished[0] = True          # run_tests() is complete
            uc_.emu_stop()
            return True                 # ...and this access is not an error
        pc = uc_.reg_read(UC_ARM_REG_PC)
        where = "?"
        for name, addr in sorted(syms.items(), key=lambda kv: -kv[1]):
            if addr <= pc:
                where = f"{name}+0x{pc - addr:x}"
                break
        abort_info[:] = [access, address, size]
        print(f"ABORTED: unexpected memory access (type {access}) at 0x{address:08X}, {size} byte(s), pc=0x{pc:08X} ({where})")
        return False

    for hook in (UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_FETCH_UNMAPPED):
        uc.hook_add(hook, on_unmapped)

    uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
    # The byte-wise flash model takes a few hundred instructions per byte programmed and
    # compared: the complete NE301 download (9.4 MB) is of the order of 10^9
    try:
        uc.emu_start(entry | 1, 0, count=20_000_000_000)
    except UcError:
        # An aborted run (unexpected memory access) is reported below with the log written
        # so far. The normal end is the read of the landmark, which Unicorn 2.x also
        # reports as an error although the hook asked for a stop.
        if not finished[0]:
            print(f"ABORTED: unexpected memory access (type {abort_info[0]}) at 0x{abort_info[1]:08X}, "
                  f"{abort_info[2]} byte(s)")

    log = bytes(uc.mem_read(syms["test_log"], 16384)).split(b"\x00")[0].decode("utf-8", "replace")
    failures = struct.unpack("<i", uc.mem_read(syms["test_failures_count"], 4))[0]

    # One test per line, followed by its failures, whatever the order the log was written in
    current, blocks = None, []
    for line in log.split("\n"):
        if line.startswith("  FAIL"):
            if blocks:
                blocks[-1].append(line)
        elif line:
            current = [line]
            blocks.append(current)
    for block in blocks:
        print(("PASS " if len(block) == 1 else "FAIL ") + block[0])
        for fail in block[1:]:
            print("     " + fail.strip())
    if not finished[0]:
        # aborted: say where the run stopped
        print(f"\nABORTED before the end ({failures} failed check(s) so far)")
        return 2
    print(f"\n{failures} failed check(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
