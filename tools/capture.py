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
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.EnumWindows.restype = wintypes.BOOL
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetClassNameW.restype = ctypes.c_int
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.IsIconic.argtypes = [wintypes.HWND]
user32.IsIconic.restype = wintypes.BOOL
user32.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p
user32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
user32.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
user32.GetWindowDpiAwarenessContext.argtypes = [wintypes.HWND]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetClientRect.restype = wintypes.BOOL
user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.ReleaseDC.restype = ctypes.c_int
user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
user32.PrintWindow.restype = wintypes.BOOL
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HANDLE]
gdi32.SelectObject.restype = wintypes.HANDLE
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                          ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
gdi32.GetDIBits.restype = ctypes.c_int
gdi32.DeleteObject.argtypes = [wintypes.HANDLE]
gdi32.DeleteObject.restype = wintypes.BOOL
gdi32.DeleteDC.argtypes = [wintypes.HDC]
gdi32.DeleteDC.restype = wintypes.BOOL


def win32_failure(operation: str) -> None:
    sys.exit(f"{operation} failed (Win32 error {ctypes.get_last_error()})")


def find_window(pid: int) -> int:
    found = []

    @WNDENUMPROC
    def cb(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, name, 64)
        if owner.value == pid and name.value == "UnrealWindow" and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    if not user32.EnumWindows(cb, 0):
        win32_failure("EnumWindows")
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
    if user32.IsIconic(hwnd):
        sys.exit(f"capture skipped: {inst['name']} window is minimized")
    dpi_context = user32.GetWindowDpiAwarenessContext(hwnd)
    if not dpi_context:
        win32_failure("GetWindowDpiAwarenessContext")
    old = user32.SetThreadDpiAwarenessContext(dpi_context)
    if not old:
        win32_failure("SetThreadDpiAwarenessContext")
    hdc_win = hdc = bmp = old_bmp = None
    try:
        rect = wintypes.RECT()
        if not user32.GetClientRect(hwnd, ctypes.byref(rect)):
            win32_failure("GetClientRect")
        w, h = rect.right - rect.left, rect.bottom - rect.top
        if w <= 1 or h <= 1:
            sys.exit(f"capture skipped: {inst['name']} has invalid client size {w}x{h}")
        hdc_win = user32.GetDC(hwnd)
        if not hdc_win:
            win32_failure("GetDC")
        hdc = gdi32.CreateCompatibleDC(hdc_win)
        if not hdc:
            win32_failure("CreateCompatibleDC")
        bmp = gdi32.CreateCompatibleBitmap(hdc_win, w, h)
        if not bmp:
            win32_failure("CreateCompatibleBitmap")
        selected = gdi32.SelectObject(hdc, bmp)
        if not selected or selected == ctypes.c_void_p(-1).value:
            win32_failure("SelectObject")
        old_bmp = selected
        if user32.IsIconic(hwnd):
            sys.exit(f"capture skipped: {inst['name']} window was minimized")
        if not user32.PrintWindow(hwnd, hdc, 0x1 | 0x2):
            win32_failure("PrintWindow")

        class BITMAPINFOHEADER(ctypes.Structure):
            _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                        ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD),
                        ("biXPelsPerMeter", wintypes.LONG), ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                        ("biClrImportant", wintypes.DWORD)]

        bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        buf = ctypes.create_string_buffer(w * h * 4)
        # GetDIBits requires the bitmap to be unselected from its DC.
        selected = gdi32.SelectObject(hdc, old_bmp)
        if not selected or selected == ctypes.c_void_p(-1).value:
            win32_failure("SelectObject restore")
        old_bmp = None
        if gdi32.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bi), 0) != h:
            win32_failure("GetDIBits")
    finally:
        if old_bmp:
            gdi32.SelectObject(hdc, old_bmp)
        if hdc:
            gdi32.DeleteDC(hdc)
        if bmp:
            gdi32.DeleteObject(bmp)
        if hdc_win:
            user32.ReleaseDC(hwnd, hdc_win)
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
