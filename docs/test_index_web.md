# Web Manager tests — incremental reindex validation

Reproducible manual checklist for validating the `fix/incremental-reindex` changes
using the browser-based Web Manager at <https://cidvonhighwind.github.io/microreader/>.

These tests exercise the same firmware code paths as `tools/test_index.py` but
through the actual Web Serial API, which is the path real users take.

## Setup

1. Flash the device with the `fix/incremental-reindex` firmware.
2. Open <https://cidvonhighwind.github.io/microreader/> in Chrome/Edge/Firefox.
3. Click **Connect** and select the ESP32 device.
4. In a separate terminal, keep `python tools/serial_cmd.py --port COM4` ready
   for verification via the `books` command.

For each test:
- Use the Web Manager UI to perform the operation.
- Verify the result with `serial_cmd.py` → `books` (runs CMND 'L').
- Optionally take a photo of the e-ink display to confirm the menu refreshed
  in place (the 4.7 auto-refresh fix).

---

## Test 1 — Upload via Web Manager, menu refreshes in place (B1 + 4.7)

**Validates:** CMND 'W' now triggers index update; MainMenu auto-refreshes.

1. From the Web Manager, navigate to `/sdcard/books`.
2. Drag a `.epub` file (any small one) onto the drop zone.
3. Wait for "OK" in the Web Manager log.
4. **Verify:** run `books` in serial_cmd.py → the uploaded file appears.
5. **Verify (optional):** if the device was showing the main menu, the list
   refreshed without requiring navigation. If it was elsewhere, navigating
   back to the menu shows the new book.

**Expected:** book appears in the index immediately.

---

## Test 2 — Upload to a non-`/books/` folder

**Validates:** `is_book_path` matches any `/sdcard/` folder, not just `/books/`.

1. From the Web Manager, navigate to `/sdcard`.
2. Click **New Folder**, name it `archive`.
3. Navigate into `/sdcard/archive`.
4. Drag a `.epub` file.
5. **Verify:** `books` in serial_cmd.py shows the book with path
   `/sdcard/archive/<name>.epub`.

---

## Test 3 — Delete via Web Manager (B4)

**Validates:** CMND 'R' no longer truncates the index.

1. Upload at least 3 books (via Test 1 or `serial_cmd.py --upload-dir`).
2. From the Web Manager, navigate to `/sdcard/books`.
3. Click the trash icon on one of the books.
4. Confirm the delete.
5. **Verify:** `books` in serial_cmd.py returns the remaining books (count
   decreased by exactly 1; others intact).
6. **Verify (important):** open the book reader, then delete another book
   from the Web Manager. The index must remain intact for the remaining
   books. This is the B4 regression — previously it wiped the entire index.

---

## Test 4 — Rename via Web Manager (B5)

**Validates:** CMND 'N' updates the index path.

1. Upload a book named `old.epub`.
2. From the Web Manager, navigate to `/sdcard/books`.
3. Click the rename icon on `old.epub`.
4. Enter `new.epub` and confirm.
5. **Verify:** `books` in serial_cmd.py shows `/sdcard/books/new.epub`
   and does NOT show `old.epub`.
6. **Verify:** the `last_open_order` was preserved if the book had been
   opened before (open it once before renaming, then check that after
   rename + menu navigation it still appears in the "Recently Opened"
   section).

---

## Test 5 — Upload a non-`.epub` file

**Validates:** `is_book_path` rejects non-book files; index untouched.

1. From the Web Manager, navigate to `/sdcard/books`.
2. Drag a `.txt` or `.png` file.
3. **Verify:** `books` in serial_cmd.py is unchanged (the file is on disk
   but not in the index — correct, since only `.epub` files are books).

---

## Test 6 — Concurrent Web Manager + serial_cmd.py

**Validates:** slot overflow handling.

1. Open both the Web Manager and `serial_cmd.py` connected to the same device
   (only one can hold the port at a time; alternate quickly).
2. Upload via Web Manager, then immediately upload via `serial_cmd.py`.
3. **Verify:** both uploads succeed; both books appear in the index.

This is a rare scenario for a single-user e-reader. If a drop happens (one
book doesn't appear in the index), the warning `"index op dropped (slot busy)"`
appears in the ESP log. Recover with **Settings → Rebuild Book Index**.

---

## Cleanup

After running the tests:

```bash
# Reset to a clean state if needed
python tools/serial_cmd.py --port COM4
# At the prompt:
> rm /sdcard/.microreader/book_index.dat
> clear
```
