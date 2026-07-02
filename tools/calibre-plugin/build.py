#!/usr/bin/env python3
"""
Portable builder for the Microreader Calibre plugin.

Produces microreader.zip with POSIX (forward-slash) entry names so the bundled
pyserial imports correctly on macOS and Linux as well as Windows.

  WHY: PowerShell's Compress-Archive writes backslash path separators into the
  archive. Windows' zipimport tolerates that, but macOS/Linux zipimport does not,
  so `import serial` fails there and the plugin can't find the device.

Usage:
  python3 build.py            # build microreader.zip
  python3 build.py --install  # build and copy into Calibre's plugins folder
"""
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "microreader.zip")

# Files/dirs to include (relative to HERE). Directories are added recursively.
INCLUDE = ["__init__.py", "serial"]

# Skip caches and compiled artifacts.
SKIP_DIRS = {"__pycache__"}
SKIP_EXT = {".pyc", ".pyo"}


def _iter_files():
    for entry in INCLUDE:
        path = os.path.join(HERE, entry)
        if os.path.isfile(path):
            yield path, entry.replace(os.sep, "/")
        elif os.path.isdir(path):
            for root, dirs, files in os.walk(path):
                dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
                for name in files:
                    if os.path.splitext(name)[1] in SKIP_EXT:
                        continue
                    full = os.path.join(root, name)
                    arc = os.path.relpath(full, HERE).replace(os.sep, "/")
                    yield full, arc


def build():
    if os.path.exists(OUT):
        os.remove(OUT)
    count = 0
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for full, arc in sorted(_iter_files(), key=lambda t: t[1]):
            # arc is guaranteed to use '/' separators.
            z.write(full, arc)
            count += 1
    print(f"Built:     {OUT}  ({count} entries)")


def install():
    # Resolve Calibre's per-user plugins directory cross-platform.
    if sys.platform == "win32":
        base = os.path.join(os.environ.get("APPDATA", ""), "calibre")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Preferences/calibre")
    else:
        base = os.path.join(
            os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")),
            "calibre",
        )
    plugins = os.path.join(base, "plugins")
    os.makedirs(plugins, exist_ok=True)
    dst = os.path.join(plugins, "Microreader.zip")
    import shutil

    shutil.copyfile(OUT, dst)
    print(f"Installed: {dst}")


if __name__ == "__main__":
    build()
    if "--install" in sys.argv[1:]:
        install()
