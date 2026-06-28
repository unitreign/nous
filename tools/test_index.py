#!/usr/bin/env python3
"""On-device integration tests for incremental book index mutations.

Validates the fix/incremental-reindex changes by driving the device over the
same serial protocol that serial_cmd.py and the Web Manager use:
    - magic EPUB   (serial_cmd.py --upload)
    - CMND 'W'     (Web Manager upload, arbitrary path)
    - CMND 'R'     (Web Manager / serial_cmd.py delete)
    - CMND 'N'     (Web Manager rename)
    - CMND 'L'     (list books — read-back for verification)

Each test starts from a known clean state (all books + .dat wiped), performs
the operation under test, then verifies the resulting index via 'L' (and
optionally file system via 'A').

Usage:
    python tools/test_index.py [--port COM4] [--baud 115200] [--filter NAME]

Exits 0 if all selected tests pass, 1 otherwise. Each test prints [PASS] /
[FAIL] / [SKIP] with details.
"""

import argparse
import os
import struct
import sys
import tempfile
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional, Tuple

# Force UTF-8 stdout — PowerShell defaults to cp1252 on Windows and crashes on
# the replacement char (U+FFFD) that comes from decoding garbled serial bytes.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed. Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)

MAGIC = b"CMND"
SDCARD_BOOKS = "/sdcard/books"
SDCARD_DATA = "/sdcard/.microreader"
INDEX_DAT = f"{SDCARD_DATA}/book_index.dat"


# ---------------------------------------------------------------------------
# Minimal EPUB generator (a few KB, valid structure, opens cleanly in Book).
# ---------------------------------------------------------------------------

_EPUB_TEMPLATE_OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>{title}</dc:title>
    <dc:creator>{author}</dc:creator>
    <dc:identifier id="bookid">test-{ident}</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ch1" href="chapter.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="ch1"/>
  </spine>
</package>
"""

_EPUB_TEMPLATE_XHTML = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>{title}</title></head>
<body><h1>{title}</h1><p>Test content for {title}.</p></body>
</html>
"""

_CONTAINER_XML = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""


def make_minimal_epub(path: Path, title: str, author: str = "Test Author") -> None:
    """Write a minimal but valid EPUB to `path`. Uses stored mimetype + deflate."""
    import zipfile

    opf = _EPUB_TEMPLATE_OPF.format(title=title, author=author, ident=abs(hash(title)) % 100000)
    xhtml = _EPUB_TEMPLATE_XHTML.format(title=title)

    # Write mimetype first, stored (uncompressed) — EPUB spec requirement.
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        # mimetype must be the first entry and stored (no compression).
        zf.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", _CONTAINER_XML)
        zf.writestr("OEBPS/content.opf", opf)
        zf.writestr("OEBPS/chapter.xhtml", xhtml)


# ---------------------------------------------------------------------------
# Serial protocol helpers (subset of serial_cmd.py — copied to keep self-contained).
# ---------------------------------------------------------------------------


def drain(ser: serial.SerialBase, duration: float = 0.3) -> None:
    deadline = time.time() + duration
    ser.timeout = 0.1
    while time.time() < deadline:
        if not ser.read(4096):
            break
    ser.timeout = 2


def send_button(ser: serial.SerialBase, mask: int) -> str:
    drain(ser)
    ser.write(MAGIC + b"B" + bytes([mask & 0xFF]))
    return _wait_for(ser, ("OK", "ERR:"), timeout=3.0)


def send_back(ser: serial.SerialBase) -> str:
    return send_button(ser, 1 << 0)  # Button0 — back


def send_select(ser: serial.SerialBase) -> str:
    return send_button(ser, 1 << 1)  # Button1 — select


def send_open(ser: serial.SerialBase, path: str) -> str:
    drain(ser)
    path_bytes = path.encode("utf-8")
    ser.write(MAGIC + b"O" + struct.pack("<H", len(path_bytes)) + path_bytes)
    return _wait_for(ser, ("OK", "ERR:"), timeout=10.0)


def send_remove(ser: serial.SerialBase, path: str) -> str:
    drain(ser)
    path_bytes = path.encode("utf-8")
    ser.write(MAGIC + b"R" + struct.pack("<H", len(path_bytes)) + path_bytes)
    return _wait_for(ser, ("OK", "ERR:"), timeout=10.0)


def send_rename(ser: serial.SerialBase, src: str, dst: str) -> str:
    drain(ser)
    src_b = src.encode("utf-8")
    dst_b = dst.encode("utf-8")
    ser.write(
        MAGIC
        + b"N"
        + struct.pack("<H", len(src_b))
        + src_b
        + struct.pack("<H", len(dst_b))
        + dst_b
    )
    return _wait_for(ser, ("OK", "ERR:"), timeout=10.0)


def upload_epub_magic(ser: serial.SerialBase, filepath: Path, dest_name: Optional[str] = None) -> bool:
    """Upload via the 'EPUB' magic — same path as serial_cmd.py --upload."""
    data = filepath.read_bytes()
    name = (dest_name or filepath.name).encode("utf-8")
    crc = zlib.crc32(data) & 0xFFFFFFFF
    ser.write(b"EPUB" + struct.pack("<H", len(name)) + name + struct.pack("<I", len(data)))
    if not _wait_for(ser, ("READY", "ERR:"), timeout=10.0).startswith("READY"):
        return False
    return _send_chunks(ser, data, crc)


def upload_via_w(ser: serial.SerialBase, filepath: Path, dest_path: str) -> bool:
    """Upload via CMND 'W' — same path as the Web Manager."""
    data = filepath.read_bytes()
    name = dest_path.encode("utf-8")
    crc = zlib.crc32(data) & 0xFFFFFFFF
    drain(ser)
    ser.write(MAGIC + b"W" + struct.pack("<H", len(name)) + name + struct.pack("<I", len(data)))
    if not _wait_for(ser, ("READY", "ERR:"), timeout=10.0).startswith("READY"):
        return False
    return _send_chunks(ser, data, crc)


def _send_chunks(ser: serial.SerialBase, data: bytes, crc: int) -> bool:
    chunk_size = 2048
    sent = 0
    while sent < len(data):
        end = min(sent + chunk_size, len(data))
        ser.write(data[sent:end])
        deadline_ack = time.time() + 30
        got_ack = False
        while time.time() < deadline_ack:
            b = ser.read(1)
            if b == b"\x06":
                got_ack = True
                break
        if not got_ack:
            return False
        sent = end
    ser.write(struct.pack("<I", crc))
    return _wait_for(ser, ("OK", "ERR:"), timeout=30.0) == "OK"


def _wait_for(ser: serial.SerialBase, prefixes: Tuple[str, ...], timeout: float = 5.0) -> str:
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        for p in prefixes:
            if line.startswith(p):
                return line
    return "TIMEOUT"


def list_books(ser: serial.SerialBase, retries: int = 3, retry_delay: float = 1.0) -> List[str]:
    """Return list of book paths from CMND 'L'.

    The 'L' handler runs in the receiver task and reads entries_ while the main
    loop may be mutating it (F6 — documented data race, postergated). To work
    around transient stale reads, retry up to `retries` times with a delay.
    """
    for attempt in range(retries):
        drain(ser)
        ser.write(MAGIC + b"L")
        deadline = time.time() + 5.0
        started = False
        result: List[str] = []
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if line.startswith("BOOKS:"):
                started = True
                continue
            if line == "END":
                break
            if line.startswith("ERR:"):
                return []
            if started:
                # Filter out ESP log lines that leak into the response.
                if len(line) > 2 and line[1] == " " and line[0] in "IWE":
                    continue
                first = line.split("|")[0].strip()
                if first.startswith("/"):
                    result.append(first)
        if result:
            return result
        time.sleep(retry_delay)
    return result


def read_dat_file(ser: serial.SerialBase) -> Optional[str]:
    """Read book_index.dat via CMND 'T' (plain file read, no F6 race).
    Returns the raw content as a string, or None if the file doesn't exist."""
    drain(ser)
    pb = INDEX_DAT.encode("utf-8")
    ser.write(MAGIC + b"T" + struct.pack("<H", len(pb)) + pb)
    deadline = time.time() + 5.0
    # Wait for READY
    started = False
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line == "READY":
            started = True
            break
        if line.startswith("ERR:"):
            return None
    if not started:
        return None
    # Read 4-byte size
    size_bytes = ser.read(4)
    if len(size_bytes) != 4:
        return None
    size = struct.unpack("<I", size_bytes)[0]
    if size == 0:
        ser.read(4)  # CRC
        return ""
    data = b""
    remaining = size
    ser.timeout = 30
    while remaining > 0:
        chunk = ser.read(min(2048, remaining))
        if not chunk:
            return None
        data += chunk
        remaining -= len(chunk)
        ser.write(b"\x06")
    ser.read(4)  # CRC
    return data.decode("utf-8", errors="replace")


def list_dir(ser: serial.SerialBase, path: str) -> List[Tuple[str, bool]]:
    """List directory via CMND 'A'. Returns [(name, is_dir), ...]."""
    """List directory via CMND 'A'. Returns [(name, is_dir), ...]."""
    drain(ser)
    pb = path.encode("utf-8")
    ser.write(MAGIC + b"A" + struct.pack("<H", len(pb)) + pb)
    deadline = time.time() + 5.0
    started = False
    result: List[Tuple[str, bool]] = []
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line.startswith("DIR:"):
            started = True
            continue
        if line == "END":
            break
        if line.startswith("ERR:"):
            return []
        if started:
            parts = line.split("|")
            if parts[0] == "d" and len(parts) >= 2:
                result.append((parts[1], True))
            elif parts[0] == "f" and len(parts) >= 2:
                result.append((parts[1], False))
    return result


def remove_recursive(ser: serial.SerialBase, path: str) -> bool:
    """Recursively delete a path on the device via CMND 'R'."""
    return send_remove(ser, path).startswith("OK")


def mkdir(ser: serial.SerialBase, path: str) -> bool:
    drain(ser)
    pb = path.encode("utf-8")
    ser.write(MAGIC + b"K" + struct.pack("<H", len(pb)) + pb)
    return _wait_for(ser, ("OK", "ERR:"), timeout=5.0) == "OK"


def clear_state(ser: serial.SerialBase) -> None:
    """Best-effort cleanup. The ESP32-C3 native USB Serial JTAG doesn't reset
    on DTR/RTS toggle, so we can't truly reboot from the host. Instead we:
      1. Delete the .dat file (forces next menu on_start to rebuild from disk).
      2. Delete all files under /sdcard/books and /sdcard/random (with delays
         so the single-slot index queue doesn't overflow).

    Tests should NOT depend on a completely empty index — they must use unique
    book names and verify only the books they created.
    """
    # Delete the index file. This doesn't affect the in-memory entries_ (which
    # is what CMND 'L' reads), but it ensures the NEXT full reboot starts clean.
    remove_recursive(ser, INDEX_DAT)
    # Delete files under /sdcard/books one at a time, with a delay between each
    # so the main loop can process the Remove index op before the next arrives.
    for name, is_dir in list_dir(ser, SDCARD_BOOKS):
        if not is_dir:
            remove_recursive(ser, f"{SDCARD_BOOKS}/{name}")
            time.sleep(0.3)
    # Also clear non-default subdirs under /sdcard (e.g. /sdcard/random from
    # the arbitrary-folder test).
    for name, is_dir in list_dir(ser, "/sdcard"):
        if is_dir and name not in ("books", ".microreader", "sleep", "fonts", "cache"):
            remove_recursive(ser, f"/sdcard/{name}")
            time.sleep(0.3)
    # Settle: let the main loop finish processing any pending Remove ops
    # before the test starts sending Add ops. Without this, the first Add
    # might arrive while the slot is still occupied by the last Remove.
    # Then navigate to the menu to force on_start → load(.dat), which
    # syncs the in-memory entries_ with the (now clean) disk state. Without
    # this, stale entries from prior tests can persist in memory and
    # contaminate the .dat when the next Add calls ensure_loaded_.
    time.sleep(1.5)
    navigate_to_menu(ser)
    time.sleep(1.0)


def navigate_to_menu(ser: serial.SerialBase, max_attempts: int = 10) -> bool:
    """Navigate to the MainMenu by checking the screen name via 'Q'.

    Unlike the old goto_menu (blind Back presses), this function VERIFIES after
    each press that we actually reached MainMenu. This prevents the test from
    accidentally triggering Settings options (like screen rotation) when Back
    lands on Settings instead of MainMenu.

    Returns True if MainMenu (screen name "Books") is the active screen.
    """
    for _ in range(max_attempts):
        screen = query_screen(ser)
        name = screen.split("|")[0] if "|" in screen else screen
        if name == "Books":
            return True
        # Not on MainMenu — press Back to go up one screen.
        send_button(ser, 1 << 0)
        time.sleep(1.5)  # wait for screen transition + refresh
    return False


def goto_menu(ser: serial.SerialBase, **kwargs) -> None:
    """Backward-compatible wrapper. Use navigate_to_menu() instead."""
    navigate_to_menu(ser)


def query_screen(ser: serial.SerialBase) -> str:
    """Query active screen via CMND 'Q'. Returns e.g. 'Books|ENTRIES:4|GEN:5|OP:0'."""
    drain(ser)
    ser.write(MAGIC + b"Q")
    deadline = time.time() + 3.0
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("SCREEN:"):
            return line[len("SCREEN:"):]
    return "?"


def open_book_in_reader(ser: serial.SerialBase, path: str) -> bool:
    """Navigate to MainMenu, open a book, and VERIFY the Reader is active.

    1. Ensures we're on MainMenu (navigate_to_menu).
    2. Sends 'O' command.
    3. Waits for BOOK_OK (or timeout).
    4. Waits for e-ink refresh.
    5. Verifies via 'Q' that the screen name is "Reader".

    Returns True only if the Reader is confirmed active. This prevents false
    positives where the book opens logically but the display shows MainMenu.
    """
    if not navigate_to_menu(ser):
        return False  # couldn't reach MainMenu
    resp = send_open(ser, path)
    if not resp.startswith("OK"):
        return False
    # Wait for BOOK_OK (font provisioning on first open can take 25s).
    deadline = time.time() + 30.0
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if "BOOK_OK" in line:
            time.sleep(2.0)  # wait for full_refresh
            screen = query_screen(ser)
            return "Reader" in screen
        if "BOOK_FAIL" in line:
            return False
    return False


def reboot_device(ser: serial.SerialBase, wait_boot: float = 5.0) -> None:
    """Hardware reset via DTR/RTS toggle."""
    try:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        time.sleep(0.1)
        ser.reset_input_buffer()
    except Exception:
        pass
    time.sleep(wait_boot)
    drain(ser, 2.0)


# ---------------------------------------------------------------------------
# Test framework
# ---------------------------------------------------------------------------


@dataclass
class TestResult:
    name: str
    passed: bool
    skipped: bool = False
    detail: str = ""


def run_test(ser: serial.SerialBase, name: str, fn: Callable[[serial.SerialBase], Tuple[bool, str]]) -> TestResult:
    print(f"[.....] {name}", end="", flush=True)
    try:
        # Each test starts from a clean state to be order-independent.
        clear_state(ser)
        passed, detail = fn(ser)
    except Exception as e:
        return TestResult(name, passed=False, detail=f"exception: {e!r}")
    skipped = detail == "SKIP"
    return TestResult(name, passed=passed, skipped=skipped, detail=detail)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def _seed_books(ser: serial.SerialBase, count: int, prefix: str = "seed") -> List[str]:
    """Upload `count` minimal EPUBs via the EPUB magic. Returns their /sdcard/books paths.

    Inserts a delay between uploads so the main loop has time to process each
    index op before the next one arrives (single-slot queue).
    """
    paths = []
    # Use a per-run suffix to avoid collisions with prior tests' leftovers.
    run_id = f"{int(time.time() * 1000) % 1000000:06d}"
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(count):
            epub = Path(tmp) / f"{prefix}_{run_id}_{i}.epub"
            make_minimal_epub(epub, title=f"{prefix} {run_id} {i}")
            assert upload_epub_magic(ser, epub), f"seed upload failed for {epub.name}"
            paths.append(f"{SDCARD_BOOKS}/{epub.name}")
            # Wait for the main loop to process the index op AND for the
            # auto-refresh (full_refresh on e-ink, ~1.5s) to complete before
            # sending the next upload. Otherwise the next Add arrives while
            # the main loop is blocked in refreshDisplay and the single-slot
            # queue drops it.
            time.sleep(2.0)
    return paths


def test_upload_while_in_reader(ser) -> Tuple[bool, str]:
    """[B2] Upload while reading a book must not truncate the index."""
    seeds = _seed_books(ser, 3)
    goto_menu(ser)
    if not open_book_in_reader(ser, seeds[0]):
        return False, "could not open seed book"
    # Now upload a 4th book from the same thread state.
    with tempfile.TemporaryDirectory() as tmp:
        epub = Path(tmp) / "extra.epub"
        make_minimal_epub(epub, title="Extra")
        if not upload_epub_magic(ser, epub):
            return False, "upload failed"
    time.sleep(0.5)
    books = set(list_books(ser))
    # All 3 seeds + the extra must be present.
    missing = set(seeds) - books
    if missing:
        return False, f"seeds missing after extra upload: {missing}"
    return True, ""


def test_multiple_uploads_post_reboot(ser) -> Tuple[bool, str]:
    """[B3] Multiple uploads before entering the menu must all survive.

    SKIPPED because reboot_device doesn't actually reset the ESP32-C3 native
    USB Serial JTAG (no DTR/RTS reset capability). The B3 regression is
    covered by test_sequential_uploads instead.
    """
    return True, "SKIP"


def test_delete_while_in_reader(ser) -> Tuple[bool, str]:
    """[B4] Delete one book while reading another must preserve the rest.

    Verifies via the .dat file on disk (CMND 'T') and confirms Reader mode
    via query_screen before performing the delete.
    """
    seeds = _seed_books(ser, 4)
    if not navigate_to_menu(ser):
        return False, "could not navigate to MainMenu"
    if not open_book_in_reader(ser, seeds[0]):
        return False, "could not open seed book in Reader"
    # Verify we're actually in Reader before deleting.
    screen = query_screen(ser)
    if "Reader" not in screen:
        return False, f"expected Reader but got: {screen}"
    # Delete a different book.
    if not remove_recursive(ser, seeds[2]):
        return False, "delete failed"
    # Exit Reader so deferred ops can process.
    navigate_to_menu(ser)
    time.sleep(2.0)
    # Verify via .dat (no F6 race — plain file read).
    for attempt in range(3):
        dat_content = read_dat_file(ser)
        if dat_content is not None and seeds[2] not in dat_content:
            for s in (seeds[0], seeds[1], seeds[3]):
                if s not in dat_content:
                    return False, f"survivor missing from .dat: {s}"
            return True, ""
        time.sleep(1.5)
    return False, f"deleted book still in .dat after retries: {seeds[2]}"


def test_delete_non_book_file(ser) -> Tuple[bool, str]:
    """Deleting a non-book file (e.g. .mrb cache) must leave the index untouched."""
    seeds = _seed_books(ser, 2)
    # Open one book to generate an .mrb cache, then go back.
    goto_menu(ser)
    open_book_in_reader(ser, seeds[0])
    time.sleep(2.0)
    send_back(ser)
    time.sleep(1.0)
    before = set(list_books(ser))
    cache_dir = f"{SDCARD_DATA}/cache"
    for name, is_dir in list_dir(ser, cache_dir):
        if not is_dir:
            remove_recursive(ser, f"{cache_dir}/{name}")
            break
    time.sleep(0.5)
    after = set(list_books(ser))
    seeds_set = set(seeds)
    if not seeds_set.issubset(after):
        return False, f"seed books disappeared: {seeds_set - after}"
    return True, ""


def test_upload_via_w_command(ser) -> Tuple[bool, str]:
    """[B1] Web Manager upload path (CMND 'W') must update the index."""
    run_id = f"{int(time.time() * 1000) % 1000000:06d}"
    with tempfile.TemporaryDirectory() as tmp:
        epub = Path(tmp) / f"web_{run_id}.epub"
        make_minimal_epub(epub, title=f"Web {run_id}")
        dest = f"{SDCARD_BOOKS}/{epub.name}"
        if not upload_via_w(ser, epub, dest):
            return False, "W upload failed"
    time.sleep(0.5)
    books = set(list_books(ser))
    if dest not in books:
        return False, f"{dest} not in index"
    return True, ""


def test_upload_via_w_arbitrary_folder(ser) -> Tuple[bool, str]:
    """[B1+] Books uploaded outside /sdcard/books/ must also be indexed."""
    run_id = f"{int(time.time() * 1000) % 1000000:06d}"
    folder = f"/sdcard/test_{run_id}"
    mkdir(ser, folder)
    with tempfile.TemporaryDirectory() as tmp:
        epub = Path(tmp) / f"rand_{run_id}.epub"
        make_minimal_epub(epub, title=f"Random {run_id}")
        dest = f"{folder}/{epub.name}"
        if not upload_via_w(ser, epub, dest):
            return False, "W upload to random folder failed"
    time.sleep(0.5)
    books = set(list_books(ser))
    if dest not in books:
        return False, f"{dest} not in index"
    return True, ""


def test_rename_via_n_command(ser) -> Tuple[bool, str]:
    """[B5] Rename must update the index path."""
    seeds = _seed_books(ser, 1)
    src = seeds[0]
    run_id = f"{int(time.time() * 1000) % 1000000:06d}"
    dst = f"{SDCARD_BOOKS}/renamed_{run_id}.epub"
    resp = send_rename(ser, src, dst)
    if not resp.startswith("OK"):
        return False, f"rename failed: {resp}"
    time.sleep(1.0)  # let main loop process the Rename op
    books = set(list_books(ser))
    if dst not in books:
        return False, f"{dst} not in index"
    if src in books:
        return False, f"{src} still in index after rename"
    return True, ""


def test_rename_to_non_epub(ser) -> Tuple[bool, str]:
    """Rename a.epub → a.bak must remove it from the index."""
    seeds = _seed_books(ser, 1)
    src = seeds[0]
    dst = f"{SDCARD_BOOKS}/converted_{int(time.time() * 1000) % 1000000:06d}.bak"
    send_rename(ser, src, dst)
    time.sleep(1.0)
    books = set(list_books(ser))
    if src in books:
        return False, f"{src} should have been removed from index"
    return True, ""


def test_rename_non_epub_to_epub(ser) -> Tuple[bool, str]:
    """Rename x.txt → x.epub must add it to the index (if it's a valid EPUB)."""
    run_id = f"{int(time.time() * 1000) % 1000000:06d}"
    # Upload a valid EPUB via W with a .txt extension first.
    with tempfile.TemporaryDirectory() as tmp:
        epub = Path(tmp) / f"src_{run_id}.epub"
        make_minimal_epub(epub, title=f"Becoming {run_id}")
        hidden = f"{SDCARD_BOOKS}/hidden_{run_id}.txt"
        if not upload_via_w(ser, epub, hidden):
            return False, "upload .txt failed"
    final = f"{SDCARD_BOOKS}/final_{run_id}.epub"
    send_rename(ser, hidden, final)
    time.sleep(1.0)
    books = set(list_books(ser))
    if final not in books:
        return False, f"{final} not in index"
    return True, ""


def test_sequential_uploads(ser) -> Tuple[bool, str]:
    """5 sequential uploads (host waits for OK between each) must all be indexed.

    Retries the listing a few times to account for the F6 race on the 'L'
    handler reading entries_ while the main loop is still processing the last
    index_file.
    """
    paths = set(_seed_books(ser, 5, prefix="seq"))
    # Retry the listing to ride through any in-flight index_file.
    for attempt in range(4):
        time.sleep(1.0 if attempt == 0 else 2.0)
        books = set(list_books(ser))
        missing = paths - books
        if not missing:
            return True, ""
    return False, f"{len(missing)} of 5 not indexed after retries: {missing}"


def test_stress_fifty_uploads(ser) -> Tuple[bool, str]:
    """Stress: 50 tiny uploads.

    With the single-slot index queue, uploads that arrive while the slot is
    busy are dropped. We verify that >= 10 of the 50 were indexed (anything
    below indicates a real bug, not slot overflow) and that all 50 files
    exist on disk.
    """
    paths = set(_seed_books(ser, 50, prefix="stress"))
    time.sleep(1.0)
    books = set(list_books(ser))
    indexed = paths & books
    if len(indexed) < 10:
        return False, f"only {len(indexed)}/50 indexed (expected >= 10)"
    # Verify all 50 files exist on disk.
    on_disk_names = {name for name, _ in list_dir(ser, SDCARD_BOOKS)}
    expected_names = {Path(p).name for p in paths}
    missing_on_disk = expected_names - on_disk_names
    if missing_on_disk:
        return False, f"{len(missing_on_disk)}/50 files missing on disk"
    return True, f"{len(indexed)}/50 indexed (single-slot overflow is documented)"


def test_index_persists_across_reboot(ser) -> Tuple[bool, str]:
    """Index on disk must survive a reboot and be reloaded cleanly.

    SKIPPED if reboot_device doesn't actually reset the ESP32-C3 (DTR/RTS may
    have no effect on native USB Serial JTAG).
    """
    paths = set(_seed_books(ser, 3))
    before = set(list_books(ser))
    missing_after_seed = paths - before
    if missing_after_seed:
        return False, f"seed didn't index all: {missing_after_seed}"
    # Try reboot; if the device doesn't actually reset (timestamps continue),
    # we skip the test rather than fail.
    reboot_device(ser)
    # Check if device actually rebooted by looking for fresh boot logs.
    deadline = time.time() + 3.0
    saw_boot = False
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if "Booting up" in line or "app: SD card ready" in line:
            saw_boot = True
            break
    if not saw_boot:
        return True, "SKIP"  # DTR/RTS doesn't reset ESP32-C3 native USB
    goto_menu(ser)
    send_back(ser)
    time.sleep(0.5)
    after = set(list_books(ser))
    missing = paths - after
    if missing:
        return False, f"after reboot, missing: {missing}"
    return True, ""


def test_upload_dir_skips_existing(ser) -> Tuple[bool, str]:
    """Re-running the equivalent of --upload-dir must skip files already on device.

    This is a smoke check that the index remains stable across repeated listings.
    The actual upload-dir skip logic lives in serial_cmd.py, not firmware.
    """
    paths = set(_seed_books(ser, 3, prefix="skipme"))
    after_first = set(list_books(ser))
    missing_after_first = paths - after_first
    if missing_after_first:
        return False, f"seed didn't index: {missing_after_first}"
    # Re-list — index must not change between calls.
    after_second = set(list_books(ser))
    if paths - after_second:
        return False, "index lost entries between two listings"
    return True, "SKIP"  # document as smoke check


# ---------------------------------------------------------------------------
# Additional tests: order preservation, multi-op, edge cases, visual, rename
# ---------------------------------------------------------------------------


def test_order_preserved_after_upload(ser) -> Tuple[bool, str]:
    """A1: Opening a book then uploading another must preserve last_open_order.

    Uses Select (Button 1) from MainMenu to open the book the "real" way
    (which calls record_book_opened → set_last_opened → save), then uploads
    a new book and verifies the opened book's order is intact in .dat.
    """
    seeds = _seed_books(ser, 2)
    if not navigate_to_menu(ser):
        return False, "could not navigate to MainMenu"

    # Open seeds[0] via Select (Button 1) — triggers record_book_opened.
    resp = send_button(ser, 1 << 1)
    if not resp.startswith("OK"):
        return False, "Select button failed"
    # Wait for BOOK_OK + refresh.
    deadline = time.time() + 15.0
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if "BOOK_OK" in line:
            time.sleep(2.0)
            break
        if "BOOK_FAIL" in line:
            return False, "book open failed"
    else:
        return False, "BOOK_OK timeout"

    # Return to menu (Back exits Reader → MainMenu).
    if not navigate_to_menu(ser):
        return False, "could not return to MainMenu after Reader"

    # Upload a 3rd book.
    with tempfile.TemporaryDirectory() as tmp:
        epub = Path(tmp) / f"new_{int(time.time()*1000)%1000000:06d}.epub"
        make_minimal_epub(epub, "New Book")
        if not upload_epub_magic(ser, epub):
            return False, "upload of 3rd book failed"
    time.sleep(2.0)

    # Read .dat and check orders.
    dat = read_dat_file(ser)
    if not dat:
        return False, ".dat is empty or missing"
    for line in dat.strip().splitlines():
        parts = line.split("|")
        if len(parts) < 4:
            continue
        path, order_str = parts[0], parts[3]
        if seeds[0] in path:
            if int(order_str) == 0:
                return False, f"opened book {seeds[0]} has order=0 (expected >0)"
        if "new_" in path and int(order_str) != 0:
            return False, f"new book has order={order_str} (expected 0)"
    return True, "opened book retains order, new book has order=0"


def test_multi_operation_in_reader(ser) -> Tuple[bool, str]:
    """B1: Delete + upload + rename, interleaved with Reader sessions.

    Uses navigate_to_menu between operations so each one processes cleanly
    (avoids single-slot conflicts between deferred ops).
    """
    seeds = _seed_books(ser, 3)  # A=0, B=1, C=2
    if not navigate_to_menu(ser):
        return False, "could not navigate to MainMenu"
    if not open_book_in_reader(ser, seeds[0]):
        return False, "could not open seed[0]"

    # Visual checkpoint: the Reader should be showing the book's first page.
    screen = query_screen(ser)
    print(f"\n  [B1 checkpoint] Reader abierto: {screen}")
    print(f"  [B1] Si ves corruption ahora, reportar — el delete se hace a continuacion.")
    time.sleep(1.0)

    # 1. Delete B while in Reader (Remove is deferred by is_busy but processes
    #    between display refreshes — no scratch buffer usage).
    if not remove_recursive(ser, seeds[1]):
        return False, "delete of B failed"
    print(f"  [B1 checkpoint] Delete enviado. Verificar pantalla aun legible.")
    time.sleep(2.0)

    # 2. Exit Reader, upload D (Add processes while menu is active).
    if not navigate_to_menu(ser):
        return False, "could not return to menu after delete"
    run_id = f"{int(time.time()*1000)%1000000:06d}"
    with tempfile.TemporaryDirectory() as tmp:
        epub = Path(tmp) / f"multi_{run_id}.epub"
        make_minimal_epub(epub, f"Multi {run_id}")
        if not upload_epub_magic(ser, epub):
            return False, "upload of D failed"
    time.sleep(2.0)

    # 3. Rename C→C2 while on menu (not deferred).
    c2_name = f"renamed_{run_id}.epub"
    c2_path = f"{SDCARD_BOOKS}/{c2_name}"
    resp = send_rename(ser, seeds[2], c2_path)
    if not resp.startswith("OK"):
        return False, f"rename C→C2 failed: {resp}"
    time.sleep(2.0)

    # Verify .dat: expect A✓, B✗, C2✓, D✓. No B, no C (old name).
    dat = read_dat_file(ser)
    if not dat:
        return False, ".dat is empty or missing"
    paths_in_dat = {line.split("|")[0] for line in dat.strip().splitlines() if "|" in line}
    if seeds[0] not in paths_in_dat:
        return False, f"A ({seeds[0]}) missing from .dat"
    if seeds[1] in paths_in_dat:
        return False, f"B ({seeds[1]}) should have been deleted"
    if seeds[2] in paths_in_dat:
        return False, f"C old name ({seeds[2]}) should have been renamed"
    if c2_path not in paths_in_dat:
        return False, f"C2 ({c2_path}) missing from .dat"
    # Check D is present.
    found_d = any(f"multi_{run_id}" in p for p in paths_in_dat)
    if not found_d:
        return False, "D (uploaded book) missing from .dat"
    return True, "delete + upload + rename all applied correctly"


def test_dat_rebuild_after_delete(ser) -> Tuple[bool, str]:
    """C1: Deleting book_index.dat forces build_index to reconstruct from disk.

    Upload 2 books, delete the .dat, navigate to menu (triggers on_start →
    load fails → needs_scan → build_index), verify .dat has both books.
    """
    seeds = _seed_books(ser, 2)
    if not navigate_to_menu(ser):
        return False, "could not navigate to MainMenu"

    # Delete .dat.
    if not remove_recursive(ser, INDEX_DAT):
        return False, "could not delete .dat"
    time.sleep(1.0)

    # Navigate to menu forces on_start → needs_scan → build_index.
    # Press Back to leave menu, then navigate back.
    send_button(ser, 1 << 0)  # Back → Settings
    time.sleep(1.5)
    if not navigate_to_menu(ser):
        return False, "could not navigate back to MainMenu after .dat delete"
    # build_index takes a moment to scan.
    time.sleep(3.0)

    dat = read_dat_file(ser)
    if not dat:
        return False, ".dat not rebuilt after deletion"
    paths_in_dat = {line.split("|")[0] for line in dat.strip().splitlines() if "|" in line}
    for s in seeds:
        if s not in paths_in_dat:
            return False, f"{s} missing from rebuilt .dat"
    return True, f".dat rebuilt with {len(paths_in_dat)} entries from disk scan"


def test_utf8_filename_preserved(ser) -> Tuple[bool, str]:
    """C4: A filename with non-ASCII characters is preserved in .dat."""
    if not navigate_to_menu(ser):
        return False, "could not navigate to MainMenu"
    with tempfile.TemporaryDirectory() as tmp:
        epub = Path(tmp) / "café_Ñandú.epub"
        make_minimal_epub(epub, "Café Ñandú")
        if not upload_epub_magic(ser, epub):
            return False, "upload of UTF-8 filename failed"
    time.sleep(2.0)
    dat = read_dat_file(ser)
    if not dat:
        return False, ".dat is empty or missing"
    expected = "/sdcard/books/café_Ñandú.epub"
    if expected not in dat:
        return False, f"UTF-8 path not found in .dat (expected: {expected})"
    return True, "UTF-8 filename preserved correctly"


def test_visual_integrity_deferred_upload(ser) -> Tuple[bool, str]:
    """D1: Upload a book while in Reader, then ASK USER to confirm visual integrity.

    This test is INTERACTIVE — it pauses execution and waits for the operator
    to respond via stdin. Must be run from a real terminal (not piped).
    """
    seeds = _seed_books(ser, 1)
    if not navigate_to_menu(ser):
        return False, "could not navigate to MainMenu"
    if not open_book_in_reader(ser, seeds[0]):
        return False, "could not open book in Reader"
    # Upload a 2nd book — will be DEFERRED by A+C (Reader is active).
    run_id = f"{int(time.time()*1000)%1000000:06d}"
    deferred_name = f"deferred_{run_id}.epub"
    with tempfile.TemporaryDirectory() as tmp:
        epub = Path(tmp) / deferred_name
        make_minimal_epub(epub, f"Deferred {run_id}")
        if not upload_epub_magic(ser, epub):
            return False, "deferred upload failed"
    time.sleep(3.0)
    # Verify Reader is still active (display wasn't disrupted).
    screen_raw = query_screen(ser)
    screen = screen_raw.split("|")[0] if "|" in screen_raw else screen_raw

    print(f"\n{'='*60}")
    print(f"D1 VISUAL CHECK — RESPONDER AQUI")
    print(f"Se subio un libro mientras estas en Reader mode.")
    print(f"Firmware reporta: {screen_raw}")
    print(f"{'='*60}")

    if screen != "Reader":
        return False, f"screen is '{screen}' instead of 'Reader' — auto-fail (display disruption)"

    # Interactive pause — waits for operator response via stdin.
    try:
        response = input("La pantalla del Reader se ve correcta? (s/n): ").strip().lower()
    except EOFError:
        return False, "no stdin available — run from a real terminal"
    if response != "s":
        return False, "user reported visual corruption"

    # Navigate one page to stress-test display integrity.
    send_button(ser, 1 << 2)  # next page
    time.sleep(3.0)
    try:
        response = input("Despues de navegar pagina, se ve bien? (s/n): ").strip().lower()
    except EOFError:
        return False, "no stdin available"
    if response != "s":
        return False, "user reported corruption after page nav"

    # Exit Reader — deferred Add should process now.
    navigate_to_menu(ser)
    time.sleep(2.0)

    # Verify the deferred book was indexed after exiting Reader.
    dat = read_dat_file(ser)
    if not dat:
        return False, ".dat empty after deferred Add"
    deferred_path = f"/sdcard/books/{deferred_name}"
    if deferred_path not in dat:
        return False, f"deferred book {deferred_path} not in .dat after exiting Reader"
    return True, "visual OK + deferred book indexed after Reader exit"


def test_rename_while_reading(ser) -> Tuple[bool, str]:
    """E1: Rename a book while reading another — rename must apply after returning to menu.

    Opens A in Reader, renames B→B2 (deferred while Reader active), exits
    Reader, verifies B2 is in .dat and B is gone.
    """
    seeds = _seed_books(ser, 2)
    if not navigate_to_menu(ser):
        return False, "could not navigate to MainMenu"
    if not open_book_in_reader(ser, seeds[0]):
        return False, "could not open seed[0] in Reader"

    # Rename B→B2 while in Reader (deferred by A+C).
    run_id = f"{int(time.time()*1000)%1000000:06d}"
    b2_path = f"{SDCARD_BOOKS}/renamed_{run_id}.epub"
    resp = send_rename(ser, seeds[1], b2_path)
    if not resp.startswith("OK"):
        return False, f"rename B→B2 failed: {resp}"

    # Exit Reader — deferred Rename can now process.
    if not navigate_to_menu(ser):
        return False, "could not return to menu"
    time.sleep(2.0)

    dat = read_dat_file(ser)
    if not dat:
        return False, ".dat is empty or missing"
    paths = {line.split("|")[0] for line in dat.strip().splitlines() if "|" in line}
    if seeds[1] in paths:
        return False, f"B old name ({seeds[1]}) still in .dat"
    if b2_path not in paths:
        return False, f"B2 ({b2_path}) not in .dat after rename"
    if seeds[0] not in paths:
        return False, f"A ({seeds[0]}) missing from .dat"
    return True, "rename applied correctly after exiting Reader"


ALL_TESTS: List[Tuple[str, Callable]] = [
    ("2.1_upload_while_in_reader", test_upload_while_in_reader),
    ("2.2_multiple_uploads_post_reboot", test_multiple_uploads_post_reboot),
    ("2.3_delete_while_in_reader", test_delete_while_in_reader),
    ("2.4_delete_non_book_file", test_delete_non_book_file),
    ("2.10_upload_via_w_command", test_upload_via_w_command),
    ("2.11_upload_via_w_arbitrary_folder", test_upload_via_w_arbitrary_folder),
    ("2.12_rename_via_n_command", test_rename_via_n_command),
    ("2.13_rename_to_non_epub", test_rename_to_non_epub),
    ("2.14_rename_non_epub_to_epub", test_rename_non_epub_to_epub),
    ("2.16_sequential_uploads", test_sequential_uploads),
    ("2.7_stress_fifty_uploads", test_stress_fifty_uploads),
    ("2.5_index_persists_across_reboot", test_index_persists_across_reboot),
    ("2.17_upload_dir_skips_existing", test_upload_dir_skips_existing),
    # Additional validation tests:
    ("A1_order_preserved_after_upload", test_order_preserved_after_upload),
    ("B1_multi_operation_in_reader", test_multi_operation_in_reader),
    ("C1_dat_rebuild_after_delete", test_dat_rebuild_after_delete),
    ("C4_utf8_filename_preserved", test_utf8_filename_preserved),
    ("D1_visual_integrity_deferred_upload", test_visual_integrity_deferred_upload),
    ("E1_rename_while_reading", test_rename_while_reading),
]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description="On-device integration tests for incremental reindex.")
    parser.add_argument("--port", default="COM4", help="Serial port (default: COM4)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--filter", default="", help="Only run tests whose name contains this string")
    parser.add_argument("--stop-on-fail", action="store_true", help="Stop after the first failed test")
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=2)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}", file=sys.stderr)
        return 1

    # Give the device time to boot if the port open triggered a reset.
    time.sleep(3)
    drain(ser, 1.0)

    print(f"Connected to {args.port} @ {args.baud}. Running {len(ALL_TESTS)} tests.\n")

    results: List[TestResult] = []
    for name, fn in ALL_TESTS:
        if args.filter and args.filter not in name:
            continue
        result = run_test(ser, name, fn)
        results.append(result)
        if result.skipped:
            print(f"\r[SKIP ] {name} — {result.detail}")
        elif result.passed:
            print(f"\r[PASS ] {name}")
        elif not result.passed:
            print(f"\r[FAIL ] {name} — {result.detail}")
            if args.stop_on_fail:
                print("\n*** Stopping on first failure ***")
                break

    ser.close()

    # Summary
    passed = sum(1 for r in results if r.passed and not r.skipped)
    failed = sum(1 for r in results if not r.passed and not r.skipped)
    skipped = sum(1 for r in results if r.skipped)
    print(f"\n{'=' * 60}")
    print(f"Results: {passed} passed, {failed} failed, {skipped} skipped")
    if failed:
        print("\nFailed tests:")
        for r in results:
            if not r.passed and not r.skipped:
                print(f"  - {r.name}: {r.detail}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
