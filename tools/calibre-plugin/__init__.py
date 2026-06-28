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
import tempfile
import time
import zlib

# Required so other plugins (e.g. DeDRM) that do `from __init__ import PLUGIN_NAME`
# don't crash when our ZIP is on sys.path.
PLUGIN_NAME = 'Microreader'


def _bootstrap():
    """Add bundled pyserial from our own ZIP to sys.path if needed."""
    try:
        import serial
        return
    except ImportError:
        pass
    try:
        from calibre.utils.config import config_dir
        zip_path = os.path.join(config_dir, 'plugins', 'Microreader.zip')
        if os.path.isfile(zip_path) and zip_path not in sys.path:
            sys.path.insert(0, zip_path)
    except Exception:
        pass

_bootstrap()

from calibre.devices.interface import DevicePlugin
from calibre.devices.errors import OpenFailed
from calibre.devices.usbms.books import Book as _Book, BookList as _BookList

# ── constants ─────────────────────────────────────────────────────────────────

_VID       = 0x303A
_PID       = 0x1001
_BAUD      = 115200
_BOOKS_DIR = '/sdcard/books'
_CHUNK     = 2048
_ACK       = b'\x06'


def _u16le(n): return struct.pack('<H', n)
def _u32le(n): return struct.pack('<I', n)
def _crc32(b): return zlib.crc32(b) & 0xFFFFFFFF


def _find_port():
    try:
        import serial.tools.list_ports
        for info in serial.tools.list_ports.comports():
            if info.vid == _VID and info.pid == _PID:
                return info.device
    except Exception:
        pass
    return None


# ── serial connection ──────────────────────────────────────────────────────────

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

    def _write(self, data):
        self._s.write(data)

    def _readline(self, timeout=5.0):
        self._s.timeout = timeout
        return self._s.readline().decode('utf-8', errors='replace').strip()

    def _expect(self, token, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._readline(timeout=min(2.0, deadline - time.time()))
            if line == token:
                return
            if line.startswith('ERR:'):
                raise IOError(f'Device error: {line!r}')
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

    def list_books(self):
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
        with open(filepath, 'rb') as f:
            data = f.read()
        nb = remote_name.encode('utf-8')
        self._write(b'EPUB' + _u16le(len(nb)) + nb + _u32le(len(data)))
        self._expect('READY', timeout=10.0)
        total, sent = len(data), 0
        while sent < total:
            chunk = data[sent:sent + _CHUNK]
            self._write(chunk)
            if self._read_exact(1, timeout=30.0) != _ACK:
                raise IOError('Bad ACK during upload')
            sent += len(chunk)
            if report_progress:
                report_progress(sent / total, f'Uploading {remote_name}')
        self._write(_u32le(_crc32(data)))
        self._expect('OK', timeout=15.0)

    def download(self, path, report_progress=None):
        """CMND 'T': download file from device, return bytes."""
        path = path.replace('\\', '/')
        pb = path.encode('utf-8')
        self._write(b'CMND' + b'T' + _u16le(len(pb)) + pb)
        self._expect('READY', timeout=5.0)
        file_size = struct.unpack('<I', self._read_exact(4, timeout=5.0))[0]
        data = b''
        remaining = file_size
        while remaining > 0:
            chunk = self._read_exact(min(_CHUNK, remaining), timeout=30.0)
            data += chunk
            remaining -= len(chunk)
            self._write(_ACK)
            if report_progress and file_size:
                report_progress(len(data) / file_size, os.path.basename(path))
        crc_b = self._read_exact(4, timeout=5.0)
        expected = struct.unpack('<I', crc_b)[0]
        actual = _crc32(data)
        if actual != expected:
            raise IOError(f'CRC mismatch: got 0x{actual:08x}, expected 0x{expected:08x}')
        return data

    def delete(self, path):
        path = path.replace('\\', '/')
        pb = path.encode('utf-8')
        self._write(b'CMND' + b'R' + _u16le(len(pb)) + pb)
        self._expect('OK', timeout=15.0)


# ── plugin ────────────────────────────────────────────────────────────────────

class MicroreaderPlugin(DevicePlugin):
    name                    = 'Microreader'
    description             = 'Send/receive EPUB books with the Microreader (ESP32 e-reader)'
    author                  = 'Patrick'
    version                 = (1, 0, 0)
    minimum_calibre_version = (5, 0, 0)
    supported_platforms     = ['windows', 'osx', 'linux']

    FORMATS                 = ['epub']
    VENDOR_ID               = [_VID]
    PRODUCT_ID              = [_PID]
    BCD                     = [None]
    DEVICE_PLUGBOARD_NAME   = 'MICROREADER'
    BACKLOADING_ERROR_MESSAGE = None  # None = "Add to library" is supported

    _conn       = None
    _port_cache = (0.0, None)
    _cached_bl  = None

    # ── detection ─────────────────────────────────────────────────────────────

    def is_usb_connected(self, devices_on_system, debug=False, only_presence=False):
        now = time.time()
        if now - self._port_cache[0] >= 5.0:
            port = _find_port()
            self._port_cache = (now, port)
        port = self._port_cache[1]
        return (True, port) if port else (False, None)

    def can_handle(self, connected_device, debug=False):
        return bool(connected_device)

    def can_handle_windows(self, usbdevice, debug=False):
        return bool(usbdevice)

    # ── connection ────────────────────────────────────────────────────────────

    def open(self, connected_device, library_uuid):
        port = connected_device or _find_port()
        if not port:
            raise OpenFailed('Microreader: device port not found')
        try:
            self._conn = _Conn(port)
            self._cached_bl = None
        except Exception as e:
            msg = str(e)
            if 'PermissionError' in msg:
                msg = f'{port} is in use by another application. Close it and try again.'
            raise OpenFailed(f'Microreader: {msg}')

    def close(self):
        if self._conn:
            self._conn.close()
            self._conn = None

    def eject(self):
        self.close()

    def startup(self):  pass
    def shutdown(self): pass

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

    # ── storage ───────────────────────────────────────────────────────────────

    def card_prefix(self, end_session=True):   return (None, None)
    def total_space(self, end_session=True):   return (0, 0, 0)
    def free_space(self, end_session=True):    return (0, 0, 0)

    # ── book list ─────────────────────────────────────────────────────────────

    def books(self, oncard=None, end_session=True):
        if oncard or self._conn is None:
            return _BookList(None, _BOOKS_DIR, self.settings())
        if self._cached_bl is not None:
            return self._cached_bl
        bl = _BookList(None, _BOOKS_DIR, self.settings())
        try:
            entries = self._conn.list_books()
        except Exception:
            return bl
        for lpath, title, authors, size, mtime in entries:
            book = _Book('', lpath.lstrip('/'), size=size)
            book.title   = title
            book.authors = authors
            book.formats = [os.path.splitext(lpath)[1][1:].upper()]
            if mtime:
                book.datetime = datetime.datetime.fromtimestamp(
                    mtime, tz=datetime.timezone.utc).timetuple()
            bl.append(book)
        self._cached_bl = bl
        return bl

    def sync_booklists(self, booklists, end_session=True):
        if booklists and booklists[0] is not None:
            self._cached_bl = booklists[0]

    # ── upload ────────────────────────────────────────────────────────────────

    def upload_books(self, files, names, on_card=None, end_session=True, metadata=None):
        reporter = getattr(self, 'report_progress', None)
        results = []
        for filepath, name in zip(files, names):
            if not name.lower().endswith('.epub'):
                name = os.path.splitext(name)[0] + '.epub'
            self._conn.upload(filepath, name, report_progress=reporter)
            results.append((f'{_BOOKS_DIR}/{name}', os.path.getsize(filepath), None))
        return results

    @staticmethod
    def add_books_to_metadata(locations, metadata, booklists):
        for (lpath, size, _), meta in zip(locations, metadata):
            book = _Book('', lpath.lstrip('/'), size=size)
            book.title   = meta.title or os.path.splitext(os.path.basename(lpath))[0]
            book.authors = list(meta.authors) if meta.authors else ['Unknown']
            booklists[0].append(book)

    @staticmethod
    def remove_books_from_metadata(books_to_remove, booklists):
        norm = {p.replace('\\', '/') for p in books_to_remove}
        for bl in booklists:
            if bl is None:
                continue
            for book in [b for b in bl if b.lpath.replace('\\', '/') in norm]:
                bl.remove(book)

    def upload_cover(self, path, filename, metadata, filepath):
        pass

    # ── download (device → library) ───────────────────────────────────────────

    def prepare_addable_books(self, paths):
        """Download each device book to a temp file; return local paths for Calibre to import."""
        reporter = getattr(self, 'report_progress', None)
        result = []
        for i, path in enumerate(paths):
            device_path = path.replace('\\', '/')
            if not device_path.startswith('/'):
                device_path = '/' + device_path
            ext = os.path.splitext(device_path)[1]
            fd, tmp_path = tempfile.mkstemp(suffix=ext)
            os.close(fd)
            try:
                def _prog(fraction, name, _i=i, _n=len(paths)):
                    if reporter:
                        reporter((_i + fraction) / _n, name)
                data = self._conn.download(device_path, report_progress=_prog)
                with open(tmp_path, 'wb') as f:
                    f.write(data)
                result.append(tmp_path)
            except Exception:
                try:
                    os.unlink(tmp_path)
                except Exception:
                    pass
                raise
        return result

    def get_file(self, path, outfile, end_session=True):
        if hasattr(path, 'lpath'):
            path = path.lpath
        path = str(path).replace('\\', '/')
        if not path.startswith('/'):
            path = '/' + path
        data = self._conn.download(path)
        outfile.write(data)

    # ── delete ────────────────────────────────────────────────────────────────

    def delete_books(self, paths, end_session=True):
        for path in paths:
            path = path.replace('\\', '/')
            if not path.startswith('/'):
                path = '/' + path
            self._conn.delete(path)


plugin = MicroreaderPlugin
