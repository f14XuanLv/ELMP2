"""Background screenshot of an instance window (F5.2 / §7.6): PrintWindow(PW_CLIENTONLY |
PW_RENDERFULLCONTENT) in the window's own DPI context, no focus change. Output:
<userDir>/Saved/ELMP/pilot/shots/<name>-<instance>.png

  python tools/capture.py dev p0-settled
"""

import argparse
import ctypes
import struct
import sys
import zlib
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import devconfig  # noqa: E402
import instance as instances  # noqa: E402

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
user32.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p
user32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
user32.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
user32.GetWindowDpiAwarenessContext.argtypes = [wintypes.HWND]


def find_window(pid: int) -> int:
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, name, 64)
        if owner.value == pid and name.value == "UnrealWindow" and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else 0


def png(path: Path, width: int, height: int, bgra: bytes) -> None:
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)
        row = bgra[y * stride:(y + 1) * stride]
        for x in range(0, stride, 4):
            rows += bytes((row[x + 2], row[x + 1], row[x]))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def capture(inst: dict, name: str) -> Path:
    pids = instances.processes(inst)
    if not pids:
        sys.exit(f"{inst['name']} is not running")
    hwnd = find_window(pids[0])
    if not hwnd:
        sys.exit("UnrealWindow not found")
    old = user32.SetThreadDpiAwarenessContext(user32.GetWindowDpiAwarenessContext(hwnd))
    try:
        rect = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rect))
        w, h = rect.right, rect.bottom
        hdc_win = user32.GetDC(hwnd)
        hdc = gdi32.CreateCompatibleDC(hdc_win)
        bmp = gdi32.CreateCompatibleBitmap(hdc_win, w, h)
        gdi32.SelectObject(hdc, bmp)
        user32.PrintWindow(hwnd, hdc, 0x1 | 0x2)

        class BITMAPINFOHEADER(ctypes.Structure):
            _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                        ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD),
                        ("biXPelsPerMeter", wintypes.LONG), ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                        ("biClrImportant", wintypes.DWORD)]

        bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        buf = ctypes.create_string_buffer(w * h * 4)
        gdi32.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(hdc)
        user32.ReleaseDC(hwnd, hdc_win)
    finally:
        user32.SetThreadDpiAwarenessContext(old)
    out = inst["saved"] / "ELMP" / "pilot" / "shots" / f"{name}-{inst['name']}.png"
    png(out, w, h, buf.raw)
    return out


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("instance")
    p.add_argument("name")
    a = p.parse_args()
    print(capture(devconfig.instance(a.instance), a.name))


if __name__ == "__main__":
    main()
