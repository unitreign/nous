"""
Calibre device plugin for Microreader (Xteink X4, ESP32-C3 e-reader).

Install:
  1. Zip this file:  zip microreader.zip __init__.py
  2. Calibre → Preferences → Plugins → Load plugin from file → select microreader.zip
  3. Restart Calibre and plug in the device.
"""

import datetime
import os
import sys
import struct
import time
import zlib

# Calibre doesn't add the plugin ZIP to sys.path automatically, so the bundled
# pyserial package wouldn't be importable. Find the ZIP via Calibre's config API
# and add it explicitly before any serial imports.
def _bootstrap():
    try:
        import serial  # already importable — nothing to do
        return
    except ImportError:
        pass
    try:
        from calibre.utils.config import config_dir
        zip_path = os.path.join(config_dir, 'plugins', 'Microreader.zip')
        if os.path.isfile(zip_path) and zip_path not in sys.path:
            sys.path.insert(0, zip_path)
            print(f'[Microreader] added bundled serial from {zip_path}')
    except Exception as e:
        print(f'[Microreader] bootstrap failed: {e}')

_bootstrap()

from calibre.devices.interface import DevicePlugin
from calibre.devices.errors import OpenFailed
from calibre.ebooks.metadata.book.base import Metadata

# ── constants ────────────────────────────────────────────────────────────────

_VID        = 0x303A
_PID        = 0x1001
_BAUD       = 115200
_BOOKS_DIR  = '/sdcard/books'
_CHUNK      = 2048
_ACK        = b'\x06'


# ── helpers ───────────────────────────────────────────────────────────────────

def _u16le(n):  return struct.pack('<H', n)
def _u32le(n):  return struct.pack('<I', n)
def _crc32(b):  return zlib.crc32(b) & 0xFFFFFFFF


def _find_port():
    """Return the first serial port whose USB VID/PID matches the device."""
    try:
        import serial.tools.list_ports
        ports = list(serial.tools.list_ports.comports())
        print(f'[Microreader] _find_port: {len(ports)} port(s)')
        for info in ports:
            print(f'[Microreader]   {info.device}: vid={info.vid!r} pid={info.pid!r} hwid={info.hwid!r}')
            if info.vid == _VID and info.pid == _PID:
                return info.device
    except Exception as e:
        print(f'[Microreader] _find_port error: {e}')
    return None


# ── serial connection wrapper ─────────────────────────────────────────────────

class _Conn:
    def __init__(self, port_name):
        import serial
        self._s = serial.Serial(port_name, _BAUD, timeout=5)
        time.sleep(0.1)
        self._s.reset_input_buffer()

    def close(self):
        try:
            self._s.close()
        except Exception:
            pass

    # raw I/O

    def _write(self, data):
        self._s.write(data)

    def _readline(self, timeout=5.0):
        self._s.timeout = timeout
        return self._s.readline().decode('utf-8', errors='replace').strip()

    def _expect(self, token, timeout=15.0):
        """Read lines until one equals token, skipping ESP log lines. Raises on ERR:."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._readline(timeout=min(2.0, deadline - time.time()))
            if line == token:
                return
            if line.startswith('ERR:'):
                raise IOError(f'Device error: {line!r}')
            # ESP_LOGI lines and other noise — skip silently
        raise TimeoutError(f'Timeout waiting for {token!r}')

    def _read_exact(self, n, timeout=30.0):
        self._s.timeout = timeout
        buf = b''
        while len(buf) < n:
            chunk = self._s.read(n - len(buf))
            if not chunk:
                raise TimeoutError(f'read_exact: wanted {n}, got {len(buf)}')
            buf += chunk
        return buf

    # protocol: CMND sub-commands

    def _cmnd(self, sub, path=None):
        """Send CMND+sub with optional length-prefixed path; return first response line."""
        pkt = b'CMND' + sub
        if path is not None:
            pb = path.encode('utf-8')
            pkt += _u16le(len(pb)) + pb
        self._write(pkt)
        return self._readline()

    def dir_list(self, path):
        """CMND 'A': returns list of {name, type, size, mtime}."""
        pb = path.encode('utf-8')
        self._write(b'CMND' + b'A' + _u16le(len(pb)) + pb)
        self._readline()  # "DIR:<path>"
        entries = []
        while True:
            line = self._readline()
            if line == 'END' or not line:
                break
            parts = line.split('|')
            if parts[0] == 'f':
                entries.append({
                    'type': 'file', 'name': parts[1],
                    'size':  int(parts[2]) if len(parts) > 2 else 0,
                    'mtime': int(parts[3]) if len(parts) > 3 else 0,
                })
            elif parts[0] == 'd':
                entries.append({'type': 'dir', 'name': parts[1], 'size': 0, 'mtime': 0})
        return entries

    def list_books(self):
        """CMND 'L': returns list of (lpath, title, authors, size, mtime) from the book index."""
        self._write(b'CMND' + b'L')
        self._readline()  # "BOOKS:"
        books = []
        while True:
            line = self._readline()
            if line in ('END', ''):
                break
            parts = line.split('|')
            lpath   = parts[0] if len(parts) > 0 else ''
            title   = parts[1] if len(parts) > 1 else os.path.splitext(os.path.basename(lpath))[0]
            authors = [parts[2]] if len(parts) > 2 and parts[2] else ['Unknown']
            size    = int(parts[3]) if len(parts) > 3 and parts[3] else 0
            mtime   = int(parts[4]) if len(parts) > 4 and parts[4] else 0
            books.append((lpath, title, authors, size, mtime))
        return books

    def upload(self, filepath, remote_name, report_progress=None):
        """Upload a file via the EPUB magic protocol → /sdcard/books/<remote_name>."""
        with open(filepath, 'rb') as f:
            data = f.read()

        nb = remote_name.encode('utf-8')
        header = b'EPUB' + _u16le(len(nb)) + nb + _u32le(len(data))
        self._write(header)
        self._expect('READY', timeout=10.0)

        total = len(data)
        sent  = 0
        while sent < total:
            chunk = data[sent:sent + _CHUNK]
            self._write(chunk)
            ack = self._read_exact(1, timeout=30.0)
            if ack != _ACK:
                raise IOError(f'Expected ACK, got {ack!r}')
            sent += len(chunk)
            if report_progress:
                report_progress(sent / total, _('Uploading') + f' {remote_name}')

        self._write(_u32le(_crc32(data)))
        self._expect('OK', timeout=15.0)

    def delete(self, path):
        """CMND 'R': recursive delete."""
        path = path.replace('\\', '/')
        pb = path.encode('utf-8')
        self._write(b'CMND' + b'R' + _u16le(len(pb)) + pb)
        self._expect('OK', timeout=15.0)

    def download(self, path):
        """CMND 'T': read file from device, returns bytes."""
        path = path.replace('\\', '/')
        pb = path.encode('utf-8')
        self._write(b'CMND' + b'T' + _u16le(len(pb)) + pb)
        self._expect('READY', timeout=5.0)
        size_b = self._read_exact(4, timeout=5.0)
        file_size = struct.unpack('<I', size_b)[0]
        data = self._read_exact(file_size, timeout=120.0)
        crc_b = self._read_exact(4, timeout=5.0)
        expected_crc = struct.unpack('<I', crc_b)[0]
        actual_crc = zlib.crc32(data) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise IOError(f'CRC mismatch on download: got 0x{actual_crc:08x}, expected 0x{expected_crc:08x}')
        return data


# ── Calibre book list ─────────────────────────────────────────────────────────

from calibre.devices.usbms.books import Book as _Book, BookList as _BookList


# ── plugin ────────────────────────────────────────────────────────────────────

class MicroreaderPlugin(DevicePlugin):
    name                  = 'Microreader'
    description           = 'Send EPUB books to the Microreader (Xteink X4 ESP32 e-reader)'
    author                = 'Patrick'
    version               = (1, 0, 0)
    minimum_calibre_version = (5, 0, 0)
    supported_platforms   = ['windows', 'osx', 'linux']

    FORMATS               = ['epub']
    VENDOR_ID             = [_VID]
    PRODUCT_ID            = [_PID]
    BCD                   = [None]
    DEVICE_PLUGBOARD_NAME = 'MICROREADER'

    _conn        = None
    _port_cache  = (0.0, None)  # (timestamp, port_name)
    _cached_bl   = None         # updated by sync_booklists after each operation

    # ── detection ────────────────────────────────────────────────────────────

    def is_usb_connected(self, devices_on_system, debug=False, only_presence=False):
        now = time.time()
        if now - self._port_cache[0] >= 5.0:
            port = _find_port()
            self._port_cache = (now, port)
            print(f'[Microreader] scan: port={self._port_cache[1]!r}')
        port = self._port_cache[1]
        return (True, port) if port else (False, None)

    def can_handle(self, connected_device, debug=False):
        return bool(connected_device)

    def can_handle_windows(self, usbdevice, debug=False):
        return self.can_handle(usbdevice, debug)

    # ── connection ───────────────────────────────────────────────────────────

    def open(self, connected_device, library_uuid):
        port = connected_device or _find_port()
        print(f'[Microreader] open: port={port!r}')
        if not port:
            raise OpenFailed('Microreader: device port not found')
        try:
            self._conn = _Conn(port)
            self._cached_bl = None  # fresh connection; invalidate any stale cache
            print(f'[Microreader] opened {port}')
        except Exception as e:
            print(f'[Microreader] open failed: {e}')
            msg = str(e)
            if 'PermissionError' in msg and '31' in msg:
                msg = (f'{port} is already in use by another application '
                       f'(e.g. the browser manager). Close it and try again.')
            raise OpenFailed(f'Microreader: {msg}')

    def close(self):
        if self._conn:
            self._conn.close()
            self._conn = None

    def eject(self):
        self.close()

    def settings(self):
        class _S:
            format_map          = ['epub']
            use_author_sort     = False
            extra_customization = None
            save_template       = '{title} - {authors}'
        return _S()

    def get_device_information(self, end_session=True):
        return ('Microreader', '1.0', '1.0', 'application/epub+zip')

    def reset(self, key='-1', log_packets=False, report_progress=None, detected_device=None):
        pass

    def set_progress_reporter(self, report_progress):
        self.report_progress = report_progress

    def startup(self):  pass
    def shutdown(self): pass

    # ── storage info ─────────────────────────────────────────────────────────

    def card_prefix(self, end_session=True):
        return (None, None)

    def total_space(self, end_session=True):
        return (0, 0, 0)

    def free_space(self, end_session=True):
        return (0, 0, 0)

    # ── book list ─────────────────────────────────────────────────────────────

    def books(self, oncard=None, end_session=True):
        if oncard:
            return _BookList(None, _BOOKS_DIR, self.settings())
        if self._conn is None:
            return _BookList(None, _BOOKS_DIR, self.settings())
        # Return Calibre's own updated list if sync_booklists() already gave us
        # one (e.g. right after a delete/upload). A fresh connection clears this.
        if self._cached_bl is not None:
            print(f'[Microreader] books(): returning cached list ({len(self._cached_bl)} books)')
            return self._cached_bl
        bl = _BookList(None, _BOOKS_DIR, self.settings())
        try:
            entries = self._conn.list_books()
        except Exception as e:
            print(f'[Microreader] books() error: {e}')
            return bl
        for lpath, title, authors, size, mtime in entries:
            # Use empty prefix so book.lpath is the full device path with the
            # leading '/' stripped (e.g. 'sdcard/books/foo.epub'). This lets
            # Calibre match the book in its internal list; we re-add '/' when
            # sending the path back to the device.
            book = _Book('', lpath, size=size)
            book.title   = title
            book.authors = authors
            if mtime:
                book.datetime = datetime.datetime.fromtimestamp(mtime, tz=datetime.timezone.utc).timetuple()
            bl.append(book)
        print(f'[Microreader] books(): fetched {len(bl)} books from device')
        self._cached_bl = bl
        return bl

    def sync_booklists(self, booklists, end_session=True):
        # Calibre passes the updated booklists after every device operation
        # (upload, delete). Cache [0] (main-memory list) so that if books() is
        # called again in the same session it returns the already-updated list
        # rather than re-querying the device and potentially seeing stale data.
        if booklists and booklists[0] is not None:
            self._cached_bl = booklists[0]
            print(f'[Microreader] sync_booklists: cached {len(booklists[0])} books')

    # ── upload ────────────────────────────────────────────────────────────────

    def upload_books(self, files, names, on_card=None, end_session=True, metadata=None):
        results = []
        reporter = getattr(self, 'report_progress', None)
        for i, (filepath, name) in enumerate(zip(files, names)):
            if not name.lower().endswith('.epub'):
                name = os.path.splitext(name)[0] + '.epub'
            self._conn.upload(filepath, name, report_progress=reporter)
            lpath = f'{_BOOKS_DIR}/{name}'
            size  = os.path.getsize(filepath)
            results.append((lpath, size, None))
        return results

    @staticmethod
    def remove_books_from_metadata(books_to_remove, booklists):
        """Remove deleted books from the in-memory device booklist."""
        print(f'[Microreader] remove_books_from_metadata: {books_to_remove!r}')
        norm = {p.replace('\\', '/') for p in books_to_remove}
        for i, bl in enumerate(booklists):
            if bl is None:
                continue
            to_remove = [b for b in bl if b.lpath.replace('\\', '/') in norm]
            print(f'[Microreader]   booklist[{i}]: {len(bl)} books, removing {len(to_remove)}')
            for book in to_remove:
                bl.remove(book)

    @staticmethod
    def add_books_to_metadata(locations, metadata, booklists):
        """Add just-uploaded books to the in-memory device booklist."""
        for (lpath, size, _), meta in zip(locations, metadata):
            book = _Book('', lpath, size=size)
            book.title   = meta.title or os.path.splitext(os.path.basename(lpath))[0]
            book.authors = list(meta.authors) if meta.authors else ['Unknown']
            booklists[0].append(book)

    def add_books_from_metadata(self, metadata, booklists, collections_attributes):
        pass

    def upload_cover(self, path, filename, metadata, filepath):
        pass  # device has no cover display

    # ── delete ────────────────────────────────────────────────────────────────

    def delete_books(self, paths, end_session=True):
        print(f'[Microreader] delete_books: {paths!r}')
        for path in paths:
            path = path.replace('\\', '/')
            if not path.startswith('/'):
                path = '/' + path
            print(f'[Microreader]   -> deleting {path!r}')
            self._conn.delete(path)
            print(f'[Microreader]   -> OK')

    # ── download ──────────────────────────────────────────────────────────────

    def get_file(self, path, outfile, end_session=True):
        path = path.replace('\\', '/')
        if not path.startswith('/'):
            path = '/' + path
        data = self._conn.download(path)
        outfile.write(data)


plugin = MicroreaderPlugin
