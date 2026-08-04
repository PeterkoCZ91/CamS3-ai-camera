"""Build-time gzip of the web UI before the LittleFS image is created.

Why
---
`data/www/` is ~135 kB of plain HTML/CSS/JS. Both the flash image and every
HTTP response carry those bytes uncompressed. Text like this compresses ~4x,
so gzipping at build time shrinks the filesystem image *and* cuts the number
of TCP segments the ESP32 has to push per page load.

How
---
The sources in `data/` are never touched. This script is registered as a
PlatformIO *pre* extra script; it stages a copy of `$PROJECT_DATA_DIR` under
`$BUILD_DIR/fsdata`, gzips the compressible files inside that copy, and then
points `PROJECT_DATA_DIR` at the staging directory. The platform builder
(`DataToBin`, espressif32/builder/main.py) resolves `$PROJECT_DATA_DIR` when
its SConscript runs, i.e. *after* pre scripts, so the replacement is picked up
by `buildfs` / `uploadfs` without any change to how those targets are invoked.
`$BUILD_DIR` lives under `.pio/`, which is already gitignored, so nothing new
shows up in `git status`.

Why only the `.gz` file is shipped (no uncompressed twin)
---------------------------------------------------------
Verified against the pinned ESPAsyncWebServer 3.10.3 in
`.pio/libdeps/m5stack-cams3/ESPAsyncWebServer/`:

* `AsyncStaticWebHandler::_searchFile()` (src/WebHandlers.cpp:139-171) probes
  `<path>.gz` and `<path>`; a missing uncompressed file is fine. Moreover
  `_tryGzipFirst` defaults to **true** (src/WebHandlerImpl.h:29), so the `.gz`
  wins even when both exist.
* `AsyncFileResponse(File, path, ...)` (src/WebResponses.cpp:785-790) adds
  `Content-Encoding: gzip` whenever the opened file ends in `.gz` while the
  requested path does not, and derives the Content-Type from the *requested*
  path -- so `/style.css` still answers `text/css`.
* `setCacheControl("public, max-age=86400")` from src/web_server.cpp keeps
  working: `handleRequest()` appends the handler's Cache-Control after building
  the response (src/WebHandlers.cpp:283-288) and the File-based response
  constructor sets none of its own. For `.gz` files the ETag is the CRC32 from
  the gzip trailer (src/WebHandlers.cpp:216-227), i.e. a content hash, so
  conditional GET / 304 keeps working and does not depend on file timestamps.

Since the library prefers `.gz` even when the plain file is present, shipping
both would cost ~4x the flash while serving the exact same bytes. Hence: only
the `.gz`.

Trade-off to be aware of: the handler does not look at `Accept-Encoding`, so a
client that cannot inflate (plain `curl` without `--compressed`) receives raw
gzip. Every browser is fine, and no firmware code reads `/www/*` off LittleFS
directly -- `serveStatic()` in src/web_server.cpp is the only consumer.

Reproducibility: maximum deflate level and a zeroed gzip header (mtime=0, no
embedded filename), so identical sources always yield identical bytes.
"""

import gzip
import os
import shutil

Import("env")  # noqa: F821 - injected by SCons

# Extensions worth compressing. Already-compressed payloads (.jpg, .png, .gz,
# fonts, ...) are copied verbatim - deflating them only wastes flash.
COMPRESS_EXT = (".html", ".htm", ".css", ".js", ".json", ".svg", ".ico", ".txt", ".xml")

# Only this subtree is compressed. It is what serveStatic() exposes over HTTP,
# so it is the only place where the browser (not the firmware) does the
# inflating. Anything else in data/ may be read straight off LittleFS by
# firmware code, which would choke on gzip - those files are copied verbatim.
COMPRESS_SUBTREE = "www/"

# Below this the gzip header/trailer overhead plus a LittleFS block make
# compression pointless.
MIN_SIZE = 256

# Only these targets consume PROJECT_DATA_DIR; skip the work for plain
# firmware builds so `pio run` stays as fast as it was.
FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}


def _gzip_file(src, dst):
    """Write src to dst.gz deterministically. Returns compressed size."""
    with open(src, "rb") as fsrc, open(dst, "wb") as raw:
        # filename="" + mtime=0 -> byte-identical output for identical input.
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as fgz:
            shutil.copyfileobj(fsrc, fgz)
    return os.path.getsize(dst)


def stage_data_dir():
    src_dir = env.subst("$PROJECT_DATA_DIR")  # noqa: F821
    if not os.path.isdir(src_dir):
        return None

    stage_dir = os.path.join(env.subst("$BUILD_DIR"), "fsdata")  # noqa: F821

    # Rebuilt from scratch every time: no stale artifacts left behind when a
    # source file is renamed or deleted.
    if os.path.isdir(stage_dir):
        shutil.rmtree(stage_dir)
    os.makedirs(stage_dir)

    total_raw = 0
    total_out = 0
    rows = []

    for root, _dirs, files in os.walk(src_dir):
        rel_root = os.path.relpath(root, src_dir)
        out_root = stage_dir if rel_root == "." else os.path.join(stage_dir, rel_root)
        os.makedirs(out_root, exist_ok=True)

        for name in sorted(files):
            src = os.path.join(root, name)
            raw_size = os.path.getsize(src)
            total_raw += raw_size
            rel = os.path.normpath(os.path.join(rel_root, name)).replace(os.sep, "/")

            compressible = (
                rel.startswith(COMPRESS_SUBTREE)
                and name.lower().endswith(COMPRESS_EXT)
                and raw_size >= MIN_SIZE
            )
            if compressible:
                dst = os.path.join(out_root, name + ".gz")
                out_size = _gzip_file(src, dst)
                if out_size >= raw_size:
                    # Compression did not pay off - ship the plain file.
                    os.remove(dst)
                    compressible = False
                else:
                    rows.append((rel + ".gz", raw_size, out_size))
                    total_out += out_size

            if not compressible:
                shutil.copy2(src, os.path.join(out_root, name))
                rows.append((rel, raw_size, raw_size))
                total_out += raw_size

    print("gzip_www: staging filesystem data in %s" % stage_dir)
    for rel, raw_size, out_size in rows:
        if out_size == raw_size:
            print("  %-32s %7d  (stored)" % (rel, raw_size))
        else:
            print(
                "  %-32s %7d -> %6d  (-%.1f%%)"
                % (rel, raw_size, out_size, 100.0 * (raw_size - out_size) / raw_size)
            )
    if total_raw:
        print(
            "gzip_www: %d bytes -> %d bytes (-%.1f%%)"
            % (total_raw, total_out, 100.0 * (total_raw - total_out) / total_raw)
        )
    return stage_dir


if FS_TARGETS & set(COMMAND_LINE_TARGETS):  # noqa: F821
    staged = stage_data_dir()
    if staged:
        # Redirect the filesystem image builder at the compressed copy.
        env.Replace(PROJECT_DATA_DIR=staged)  # noqa: F821
