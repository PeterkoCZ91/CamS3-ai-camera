#!/usr/bin/env python3
"""Put WiFi credentials onto a CamS3 over USB, without the captive portal.

Why
---
A fresh or reflashed board comes up in AP mode (`CamS3-Setup`) and wants a phone to
join it and fill in a form. That is a dead end when the machine you work from has no
WiFi adapter. The firmware reads `wifi_ssid` / `wifi_pass` from the NVS namespace
`cams3`, accepts them as plain strings (the "legacy plaintext" path) and re-encrypts
them on first boot, so an NVS image written straight to the partition is enough.

    python3 tools/provision_wifi.py --ssid MyNetwork            # asks for the password
    CAMS3_WIFI_PASS=... python3 tools/provision_wifi.py --ssid MyNetwork --port /dev/ttyACM0
    python3 tools/provision_wifi.py --ssid X --password Y --out nvs.bin --dry-run

This REPLACES the whole NVS partition: the HTTP password, MQTT and Telegram secrets
are gone too and fall back to defaults. The temporary files are overwritten and
removed. The board then joins the network by itself; find it by MAC or hostname.
"""

import argparse
import getpass
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _esp  # noqa: E402

NAMESPACE = "cams3"


def nvs_generator() -> Path:
    path = (_esp.PIO_HOME / "packages" / "framework-espidf" / "components" / "nvs_flash"
            / "nvs_partition_generator" / "nvs_partition_gen.py")
    if not path.exists():
        sys.exit(f"nvs_partition_gen.py not found at {path} (is the PlatformIO ESP-IDF package installed?)")
    return path


def shred(path: Path) -> None:
    try:
        size = path.stat().st_size
        path.write_bytes(b"\0" * size)
    except FileNotFoundError:
        pass
    finally:
        path.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--password", help="visible in the process list; prefer CAMS3_WIFI_PASS or the prompt")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", help="keep the generated NVS image here instead of a temp file")
    ap.add_argument("--dry-run", action="store_true", help="generate the image, do not flash")
    args = ap.parse_args()

    password = args.password or os.environ.get("CAMS3_WIFI_PASS") or getpass.getpass("WiFi password: ")
    for label, value in (("ssid", args.ssid), ("password", password)):
        if not value or "," in value or '"' in value or "\n" in value:
            sys.exit(f"{label} is empty or contains a comma, quote or newline (not supported by the CSV step)")

    offset, size = _esp.partitions()["nvs"]
    with tempfile.TemporaryDirectory() as tmp:
        csv = Path(tmp) / "nvs.csv"
        image = Path(args.out) if args.out else Path(tmp) / "nvs.bin"
        csv.write_text(
            "key,type,encoding,value\n"
            f"{NAMESPACE},namespace,,\n"
            f"wifi_ssid,data,string,{args.ssid}\n"
            f"wifi_pass,data,string,{password}\n"
        )
        try:
            result = subprocess.run(
                [sys.executable, str(nvs_generator()), "generate", str(csv), str(image), hex(size)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if result.returncode != 0:
                sys.exit(result.stdout.replace(password, "***").replace(args.ssid, "<ssid>"))
            print(f"NVS image: {image} ({image.stat().st_size} bytes, for {offset:#x})")
            if args.dry_run:
                return 0
            ok = _esp.write_flash(_esp.find_esptool(), args.port, args.baud, offset, str(image),
                                  after="hard_reset")
            print("written, board restarting" if ok else "flash FAILED (is the board in range of the USB port?)")
            return 0 if ok else 1
        finally:
            shred(csv)
            if not args.out:
                shred(image)


if __name__ == "__main__":
    sys.exit(main())
