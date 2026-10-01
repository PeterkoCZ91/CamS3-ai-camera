"""Shared helpers for the flash / provisioning scripts in this directory."""

import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PIO_HOME = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))


def find_esptool() -> list:
    """The esptool PlatformIO already installed, else whatever is on PATH."""
    for candidate in (PIO_HOME / "penv" / "bin" / "esptool", PIO_HOME / "penv" / "bin" / "esptool.py"):
        if candidate.exists():
            return [str(candidate)]
    for name in ("esptool", "esptool.py"):
        for directory in os.environ.get("PATH", "").split(os.pathsep):
            if directory and (Path(directory) / name).exists():
                return [str(Path(directory) / name)]
    sys.exit("esptool not found: install PlatformIO or `pip install esptool`")


def partitions(csv_path: Path = REPO / "partitions.csv") -> dict:
    """{name: (offset, size)} from the project's partition table."""
    table = {}
    for line in csv_path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) >= 5:
            table[cols[0]] = (int(cols[3], 0), int(cols[4], 0))
    return table


def run(cmd: list, log=None) -> int:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if log is not None:
        log.write(proc.stdout)
    return proc.returncode


def write_flash(esptool: list, port: str, baud: int, address: int, path: str,
                attempts: int = 8, after: str = "no_reset", log=None) -> bool:
    """One `esptool write_flash`, retried: on a flaky board the USB device
    re-enumerates mid-run and the first few connection attempts simply fail."""
    cmd = esptool + [
        "--chip", "esp32s3", "--port", port, "--baud", str(baud),
        "--before", "default_reset", "--after", after, "--connect-attempts", "20",
        "write_flash", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
        hex(address), path,
    ]
    for _ in range(attempts):
        if run(cmd, log) == 0:
            return True
    return False
