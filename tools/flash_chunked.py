#!/usr/bin/env python3
"""Flash the CamS3 in small pieces when a normal `pio run -t upload` dies.

Why
---
On some boards (seen on a CamS3 5MP, 2026-10-01) `esptool` writes the small
regions fine — bootloader, partition table, otadata — and then fails on the first
large write with "The chip stopped responding", while `erase_flash` fails the
same way. The USB device re-enumerates during the failing step. Writing the
application in 64 kB pieces, each its own short `esptool` run, got through where
the single 1.4 MB write did not.

What it writes
--------------
bootloader (0x0), partition table, boot_app0 (otadata), the application in
64 kB pieces, and — with --fs — the non-empty 64 kB pieces of the LittleFS image.
Offsets come from partitions.csv. All-0xFF filesystem pieces are skipped, so
stale bytes in those blocks stay on the flash: LittleFS only follows pointers from
its superblock, which lives in the first piece and is always written. The --fs
path has NOT been run on hardware yet — treat it as experimental and check that
the web UI loads afterwards.

    python3 tools/flash_chunked.py --port /dev/ttyACM0            # firmware
    python3 tools/flash_chunked.py --port /dev/ttyACM0 --fs       # + web UI
    python3 tools/flash_chunked.py --dry-run                      # just the plan

Build first (`pio run`, and `pio run -t buildfs` for --fs).
"""

import argparse
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _esp  # noqa: E402

CHUNK = 0x10000


def pieces(data: bytes, base: int, skip_blank: bool):
    for index in range(0, len(data), CHUNK):
        part = data[index:index + CHUNK]
        if skip_blank and part.count(0xFF) == len(part):
            continue
        yield base + index, part


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--env", default="m5stack-cams3", help="PlatformIO environment (build directory name)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--fs", action="store_true", help="also write the LittleFS image (experimental)")
    ap.add_argument("--boot-app0", help="path to boot_app0.bin (default: from the PlatformIO Arduino package)")
    ap.add_argument("--dry-run", action="store_true", help="print what would be written and exit")
    args = ap.parse_args()

    build = _esp.REPO / ".pio" / "build" / args.env
    table = _esp.partitions()
    boot_app0 = Path(args.boot_app0) if args.boot_app0 else (
        _esp.PIO_HOME / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin")

    plan = [
        (0x0, build / "bootloader.bin", False),
        (0x8000, build / "partitions.bin", False),
        (0xE000, boot_app0, False),
        (table["app0"][0], build / "firmware.bin", False),
    ]
    if args.fs:
        plan.append((table["spiffs"][0], build / "littlefs.bin", True))

    writes = []
    for base, path, skip_blank in plan:
        if not path.exists():
            sys.exit(f"missing {path} — build first")
        if path.stat().st_size > CHUNK or skip_blank:
            writes.extend((a, d, path.name) for a, d in pieces(path.read_bytes(), base, skip_blank))
        else:
            writes.append((base, path.read_bytes(), path.name))

    print(f"{len(writes)} writes:")
    for address, data, name in writes:
        print(f"  {address:#010x}  {len(data):>6} B  {name}")
    if args.dry_run:
        return 0

    esptool = _esp.find_esptool()
    log_path = _esp.REPO / ".pio" / "flash_chunked.log"
    log_path.parent.mkdir(exist_ok=True)
    with open(log_path, "w") as log, tempfile.TemporaryDirectory() as tmp:
        for number, (address, data, name) in enumerate(writes, 1):
            piece = Path(tmp) / "piece.bin"
            piece.write_bytes(data)
            if not _esp.write_flash(esptool, args.port, args.baud, address, str(piece), log=log):
                print(f"FAILED at {address:#x} ({name}); esptool output: {log_path}")
                return 1
            print(f"\r{number}/{len(writes)}", end="", flush=True)
    print("\nall pieces written and verified; resetting")
    _esp.run(esptool + ["--chip", "esp32s3", "--port", args.port, "--before", "default_reset",
                        "--after", "hard_reset", "--connect-attempts", "20", "chip_id"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
