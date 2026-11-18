# -*- coding: utf-8 -*-
"""
高性能 CPU 负载显示球（Windows）

- 实时显示 CPU 负载（GetSystemTimes，无第三方 CPU 采样依赖）
- 颜色：绿=轻载 → 黄/橙 → 红=重载
- 置顶悬浮球，可拖动
- 系统托盘（右下角）右键菜单退出
"""

from __future__ import annotations

import ctypes
import math
import os
import struct
import sys
import tempfile
import threading
import time
import traceback
from ctypes import wintypes
from typing import Optional, Tuple

# 某些 Python 构建的 wintypes 缺少这些句柄类型
for _name in ("HCURSOR", "HICON", "HMENU", "HBRUSH", "HBITMAP",
              "HINSTANCE", "HMODULE", "HMONITOR"):
    if not hasattr(wintypes, _name):
        setattr(wintypes, _name, ctypes.c_void_p)

if not hasattr(wintypes, "GUID"):
    class _GUID(ctypes.Structure):
        _fields_ = [
            ("Data1", wintypes.DWORD),
            ("Data2", wintypes.WORD),
            ("Data3", wintypes.WORD),
            ("Data4", ctypes.c_ubyte * 8),
        ]
    wintypes.GUID = _GUID

# ---------------------------------------------------------------------------
# 常量 / 配置
# ---------------------------------------------------------------------------

BALL_SIZE = 96              # 球体直径（像素）
UPDATE_MS = 200             # 刷新间隔（毫秒），约 5 次/秒
EMA_ALPHA = 0.35            # 负载平滑系数（越小越平滑）
FONT_FAMILY = "Segoe UI"
TRAY_ID = 0x1C01            # 托盘图标 UID（任意非零）
WM_TRAY = 0x8000 + 1        # 自定义托盘回调消息
IDM_EXIT = 1001
IDM_TOGGLE_TOP = 1002
IDM_TOGGLE_TEXT = 1003
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040
GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x00000008
WS_EX_LAYERED = 0x00080000
WS_EX_TOOLWINDOW = 0x00000080

# Shell / User32 常量
NIM_ADD = 0x00000000
NIM_MODIFY = 0x00000001
NIM_DELETE = 0x00000002
NIF_MESSAGE = 0x00000001
NIF_ICON = 0x00000002
NIF_TIP = 0x00000004
NIF_SHOWTIP = 0x00000080
MIIM_STATE = 0x00000001
MIIM_ID = 0x00000002
MIIM_STRING = 0x00000040
MIIM_FTYPE = 0x00000100
MFT_STRING = 0x00000000
MFT_SEPARATOR = 0x00000800
MF_BYCOMMAND = 0x00000000
MF_ENABLED = 0x00000000
MF_GRAYED = 0x00000001
MF_STRING = 0x00000000
MF_SEPARATOR = 0x00000800
TPM_RIGHTBUTTON = 0x0002
TPM_RETURNCMD = 0x0100
TPM_NONOTIFY = 0x0080
WM_COMMAND = 0x0111
WM_USER = 0x0400
WM_LBUTTONUP = 0x0202
WM_RBUTTONUP = 0x0205
WM_LBUTTONDBLCLK = 0x0203
WM_RBUTTONDOWN = 0x0204
WM_CONTEXTMENU = 0x007B
WM_DESTROY = 0x0002
WM_QUIT = 0x0012
CS_HREDRAW = 0x0002
CS_VREDRAW = 0x0001
CW_USEDEFAULT = 0x80000000
HWND_MESSAGE = -3
ICON_SMALL = 0
IMAGE_ICON = 1
LR_LOADFROMFILE = 0x0010
IDI_APPLICATION = 32512
GWLP_USERDATA = -21
PM_REMOVE = 0x0001

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)


# ---------------------------------------------------------------------------
# Windows 结构体
# ---------------------------------------------------------------------------

class FILETIME(ctypes.Structure):
    _fields_ = [
        ("dwLowDateTime", wintypes.DWORD),
        ("dwHighDateTime", wintypes.DWORD),
    ]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [
        ("style", ctypes.c_uint),
        ("lpfnWndProc", ctypes.c_void_p),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wintypes.HINSTANCE),
        ("hIcon", wintypes.HICON),
        ("hCursor", wintypes.HCURSOR),
        ("hbrBackground", wintypes.HBRUSH),
        ("lpszMenuName", wintypes.LPCWSTR),
        ("lpszClassName", wintypes.LPCWSTR),
    ]


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class MSG(ctypes.Structure):
    _fields_ = [
        ("hwnd", wintypes.HWND),
        ("message", ctypes.c_uint),
        ("wParam", wintypes.WPARAM),
        ("lParam", wintypes.LPARAM),
        ("time", wintypes.DWORD),
        ("pt", POINT),
    ]


class NOTIFYICONDATAW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("hWnd", wintypes.HWND),
        ("uID", ctypes.c_uint),
        ("uFlags", ctypes.c_uint),
        ("uCallbackMessage", ctypes.c_uint),
        ("hIcon", wintypes.HICON),
        ("szTip", wintypes.WCHAR * 128),
        ("dwState", wintypes.DWORD),
        ("dwStateMask", wintypes.DWORD),
        ("szInfo", wintypes.WCHAR * 256),
        ("uTimeout", ctypes.c_uint),
        ("szInfoTitle", wintypes.WCHAR * 64),
        ("dwInfoFlags", wintypes.DWORD),
        ("guidItem", wintypes.GUID),
        ("hBalloonIcon", wintypes.HICON),
    ]


class MENUITEMINFOW(ctypes.Structure):
    _fields_ = [
        ("cbSize", ctypes.c_uint),
        ("fMask", ctypes.c_uint),
        ("fType", ctypes.c_uint),
        ("fState", ctypes.c_uint),
        ("wID", ctypes.c_uint),
        ("hSubMenu", wintypes.HMENU),
        ("hbmpChecked", wintypes.HBITMAP),
        ("hbmpUnchecked", wintypes.HBITMAP),
        ("dwItemData", ctypes.c_size_t),
        ("dwTypeData", wintypes.LPWSTR),
        ("cch", ctypes.c_uint),
        ("hbmpItem", wintypes.HBITMAP),
    ]


# ---------------------------------------------------------------------------
# 高性能 CPU 采样（GetSystemTimes）
# ---------------------------------------------------------------------------

class CpuSampler:
    """基于 GetSystemTimes 的系统级 CPU 负载采样器。"""

    def __init__(self) -> None:
        self._prev_idle = 0
        self._prev_total = 0
        self._seeded = False
        self.sample()  # 建立基线

    @staticmethod
    def _raw() -> Tuple[int, int, int]:
        idle = FILETIME()
        kernel = FILETIME()
        user = FILETIME()
        ok = kernel32.GetSystemTimes(
            ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)
        )
        if not ok:
            raise OSError("GetSystemTimes failed")
        idle_u = (idle.dwHighDateTime << 32) | idle.dwLowDateTime
        kernel_u = (kernel.dwHighDateTime << 32) | kernel.dwLowDateTime
        user_u = (user.dwHighDateTime << 32) | user.dwLowDateTime
        # Windows: kernel 时间包含 idle
        total = kernel_u + user_u
        return idle_u, kernel_u, total

    def sample(self) -> float:
        """返回 [0, 100] 的 CPU 使用率。"""
        idle, kernel, total = self._raw()
        if not self._seeded:
            self._prev_idle = idle
            self._prev_total = total
            self._seeded = True
            return 0.0
        d_idle = idle - self._prev_idle
        d_total = total - self._prev_total
        self._prev_idle = idle
        self._prev_total = total
        if d_total <= 0:
            return 0.0
        busy = d_total - d_idle
        if busy < 0:
            busy = 0
        pct = 100.0 * busy / d_total
        return max(0.0, min(100.0, pct))


# ---------------------------------------------------------------------------
# 颜色：绿(轻) → 黄 → 橙 → 红(重)
# ---------------------------------------------------------------------------

def load_color(pct: float) -> str:
    """pct: 0-100 → 十六进制颜色。四档：0-25 绿 / 25-50 黄 / 50-75 橙 / 75-100 红。"""
    pct = max(0.0, min(100.0, pct))
    if pct < 25.0:
        r, g, b = 46, 204, 113
    elif pct < 50.0:
        r, g, b = 241, 196, 15
    elif pct < 75.0:
        r, g, b = 230, 126, 34
    else:
        r, g, b = 231, 76, 60
    return f"#{r:02x}{g:02x}{b:02x}"

def shift_color(hex_color: str, factor: float) -> str:
    """在原色基础上按 factor 亮度缩放（用于外圈/内芯）。"""
    h = hex_color.lstrip("#")
    r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    r = max(0, min(255, int(r * factor)))
    g = max(0, min(255, int(g * factor)))
    b = max(0, min(255, int(b * factor)))
    return f"#{r:02x}{g:02x}{b:02x}"


def blend_color(hex_a: str, hex_b: str, t: float) -> str:
    t = max(0.0, min(1.0, t))
    ha, hb = hex_a.lstrip("#"), hex_b.lstrip("#")
    ra, ga, ba = int(ha[0:2], 16), int(ha[2:4], 16), int(ha[4:6], 16)
    rb, gb, bb = int(hb[0:2], 16), int(hb[2:4], 16), int(hb[4:6], 16)
    r = int(ra + (rb - ra) * t)
    g = int(ga + (gb - ga) * t)
    b = int(ba + (bb - ba) * t)
    return f"#{r:02x}{g:02x}{b:02x}"


# ---------------------------------------------------------------------------
# 托盘图标（纯 Win32，无 pystray）
# ---------------------------------------------------------------------------

def _make_tray_icon_path(pct: float) -> str:
    """生成当前负载颜色的小 ICO，返回临时文件路径。"""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return ""

    size = 32
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    color = load_color(pct)
    h = color.lstrip("#")
    rgb = (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), 255)
    # 外圈深色描边
    draw.ellipse((1, 1, size - 2, size - 2), fill=(30, 30, 30, 255))
    draw.ellipse((3, 3, size - 4, size - 4), fill=rgb)
    # 高光
    draw.ellipse((7, 6, 16, 14), fill=(255, 255, 255, 70))

    fd, path = tempfile.mkstemp(prefix="cpu_ball_", suffix=".png")
    os.close(fd)
    img.save(path, format="PNG")
    # 同时写 ico（部分环境 tray 更认 ico）
    ico_path = path[:-4] + ".ico"
    try:
        img.save(ico_path, format="ICO", sizes=[(16, 16), (32, 32)])
        try:
            os.remove(path)
        except OSError:
            pass
        return ico_path
    except Exception:
        return path


class TrayIcon:
    """系统托盘：右键菜单退出。在独立线程运行消息循环。"""

    def __init__(self, on_exit, on_toggle_top, on_toggle_text, tooltip_base: str):
        self.on_exit = on_exit
        self.on_toggle_top = on_toggle_top
        self.on_toggle_text = on_toggle_text
        self.tooltip_base = tooltip_base
        self.exit_event = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._hwnd = None
        self._nid = None
        self._icon_path = ""
        self._hicon = None
        self._ready = threading.Event()
        self._tip = tooltip_base
        self._lock = threading.Lock()
        self._pending_tip: Optional[str] = None

        # 动态链接函数
        self._Shell_NotifyIconW = shell32.Shell_NotifyIconW
        self._Shell_NotifyIconW.argtypes = [wintypes.DWORD, ctypes.c_void_p]
        self._Shell_NotifyIconW.restype = wintypes.BOOL
        self._CreatePopupMenu = user32.CreatePopupMenu
        self._CreatePopupMenu.restype = wintypes.HMENU
        self._AppendMenuW = user32.AppendMenuW
        self._AppendMenuW.argtypes = [
            wintypes.HMENU, ctypes.c_uint, ctypes.c_size_t, wintypes.LPCWSTR
        ]
        self._AppendMenuW.restype = wintypes.BOOL
        self._TrackPopupMenu = user32.TrackPopupMenu
        self._TrackPopupMenu.restype = ctypes.c_uint
        self._DestroyMenu = user32.DestroyMenu
        self._DestroyMenu.argtypes = [wintypes.HMENU]
        self._DestroyMenu.restype = wintypes.BOOL
        self._GetCursorPos = user32.GetCursorPos
        self._GetCursorPos.argtypes = [ctypes.POINTER(POINT)]
        self._PostMessageW = user32.PostMessageW
        self._PostMessageW.argtypes = [
            wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM
        ]
        self._LoadImageW = user32.LoadImageW
        self._LoadImageW.argtypes = [
            wintypes.HINSTANCE, wintypes.LPCWSTR, ctypes.c_uint,
            ctypes.c_int, ctypes.c_int, ctypes.c_uint,
        ]
        self._LoadImageW.restype = wintypes.HICON
        self._LoadIconW = user32.LoadIconW
        self._LoadIconW.argtypes = [wintypes.HINSTANCE, wintypes.LPCWSTR]
        self._LoadIconW.restype = wintypes.HICON
        self._DestroyIcon = user32.DestroyIcon
        self._DestroyIcon.argtypes = [wintypes.HICON]
        self._DestroyIcon.restype = wintypes.BOOL
        self._GetModuleHandleW = kernel32.GetModuleHandleW
        self._GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
        self._GetModuleHandleW.restype = wintypes.HMODULE
        self._CreateWindowExW = user32.CreateWindowExW
        self._CreateWindowExW.argtypes = [
            ctypes.c_uint, wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_uint,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID,
        ]
        self._CreateWindowExW.restype = wintypes.HWND
        self._RegisterClassW = user32.RegisterClassW
        self._RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
        self._RegisterClassW.restype = wintypes.ATOM
        self._DefWindowProcW = user32.DefWindowProcW
        self._DefWindowProcW.argtypes = [
            wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM
        ]
        self._DefWindowProcW.restype = wintypes.LPARAM
        self._DestroyWindow = user32.DestroyWindow
        self._DestroyWindow.argtypes = [wintypes.HWND]
        self._DestroyWindow.restype = wintypes.BOOL
        self._GetMessageW = user32.GetMessageW
        self._GetMessageW.argtypes = [
            ctypes.POINTER(MSG), wintypes.HWND, ctypes.c_uint, ctypes.c_uint
        ]
        self._GetMessageW.restype = ctypes.c_int
        self._TranslateMessage = user32.TranslateMessage
        self._DispatchMessageW = user32.DispatchMessageW
        self._PostQuitMessage = user32.PostQuitMessage
        self._PostQuitMessage.argtypes = [ctypes.c_int]

        # WNDPROC
        self._WNDPROC = ctypes.WINFUNCTYPE(
            wintypes.LPARAM, wintypes.HWND, ctypes.c_uint,
            wintypes.WPARAM, wintypes.LPARAM,
        )
        self._wnd_proc_ref = self._WNDPROC(self._wnd_proc)

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="cpu-ball-tray", daemon=True)
        self._thread.start()
        self._ready.wait(timeout=3.0)

    def update_tip(self, pct: float) -> None:
        tip = f"{self.tooltip_base}  {pct:.0f}%"
        with self._lock:
            self._pending_tip = tip
        hwnd = self._hwnd
        if hwnd:
            self._PostMessageW(hwnd, WM_TRAY + 1, 0, 0)  # custom refresh

    def stop(self) -> None:
        hwnd = self._hwnd
        if hwnd:
            self._PostMessageW(hwnd, WM_DESTROY, 0, 0)
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2.0)

    def _load_icon(self) -> Optional[int]:
        path = _make_tray_icon_path(0.5)
        self._icon_path = path
        if path and os.path.isfile(path):
            h = self._LoadImageW(
                None, path, IMAGE_ICON, 16, 16, LR_LOADFROMFILE
            )
            if h:
                return h
        # 回退：系统默认图标
        return self._LoadIconW(None, ctypes.c_wchar_p(IDI_APPLICATION))

    def _run(self) -> None:
        try:
            hinstance = self._GetModuleHandleW(None)
            class_name = "CpuBallTrayClass"
            wndclass = WNDCLASSW()
            wndclass.style = 0
            wndclass.lpfnWndProc = ctypes.cast(self._wnd_proc_ref, ctypes.c_void_p).value
            wndclass.cbClsExtra = 0
            wndclass.cbWndExtra = 0
            wndclass.hInstance = hinstance
            wndclass.hIcon = None
            wndclass.hCursor = None
            wndclass.hbrBackground = None
            wndclass.lpszMenuName = None
            wndclass.lpszClassName = class_name
            atom = self._RegisterClassW(ctypes.byref(wndclass))
            # 忽略已注册错误
            hwnd = self._CreateWindowExW(
                0, class_name, "CpuBallTray", 0,
                0, 0, 0, 0,
                HWND_MESSAGE, None, hinstance, None,
            )
            if not hwnd:
                self._ready.set()
                return
            self._hwnd = hwnd

            self._hicon = self._load_icon()
            nid = NOTIFYICONDATAW()
            nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
            nid.hWnd = hwnd
            nid.uID = TRAY_ID
            nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP
            nid.uCallbackMessage = WM_TRAY
            nid.hIcon = self._hicon
            nid.szTip = self.tooltip_base
            self._nid = nid
            self._Shell_NotifyIconW(NIM_ADD, ctypes.byref(nid))
            self._ready.set()

            msg = MSG()
            while True:
                ret = self._GetMessageW(ctypes.byref(msg), None, 0, 0)
                if ret == 0 or ret == -1:
                    break
                if msg.message == WM_TRAY + 1:
                    self._apply_tip()
                    continue
                self._TranslateMessage(ctypes.byref(msg))
                self._DispatchMessageW(ctypes.byref(msg))
        except Exception:
            traceback.print_exc()
            self._ready.set()
        finally:
            self._remove_icon()
            if self._icon_path and os.path.isfile(self._icon_path):
                try:
                    os.remove(self._icon_path)
                except OSError:
                    pass

    def _apply_tip(self) -> None:
        with self._lock:
            tip = self._pending_tip
        if tip is None or self._nid is None:
            return
        self._nid.szTip = tip[:127]
        self._nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP
        self._Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(self._nid))

    def _remove_icon(self) -> None:
        if self._nid is not None:
            self._Shell_NotifyIconW(NIM_DELETE, ctypes.byref(self._nid))
            self._nid = None
        if self._hicon:
            try:
                self._DestroyIcon(self._hicon)
            except Exception:
                pass
            self._hicon = None
        if self._hwnd:
            try:
                self._DestroyWindow(self._hwnd)
            except Exception:
                pass
            self._hwnd = None

    def _show_menu(self, hwnd) -> None:
        hmenu = self._CreatePopupMenu()
        if not hmenu:
            return
        try:
            self._AppendMenuW(hmenu, MF_STRING | MF_ENABLED, IDM_TOGGLE_TEXT, "显示/隐藏百分比")
            self._AppendMenuW(hmenu, MF_STRING | (0x00000008 if self.always_on_top else 0), IDM_TOGGLE_TOP, "始终置顶")
            self._AppendMenuW(hmenu, MF_SEPARATOR, 0, None)
            self._AppendMenuW(hmenu, MF_STRING | MF_ENABLED, IDM_EXIT, "退出 CPU 球")

            pt = POINT()
            self._GetCursorPos(ctypes.byref(pt))
            user32.SetForegroundWindow(hwnd)
            cmd = self._TrackPopupMenu(
                hmenu,
                TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                pt.x, pt.y, 0, hwnd, None,
            )
            if cmd == IDM_EXIT:
                if self.on_exit:
                    self.on_exit()
                self.exit_event.set()
                self._PostMessageW(hwnd, WM_QUIT, 0, 0)
            elif cmd == IDM_TOGGLE_TOP:
                if self.on_toggle_top:
                    self.on_toggle_top()
            elif cmd == IDM_TOGGLE_TEXT:
                if self.on_toggle_text:
                    self.on_toggle_text()
        finally:
            self._DestroyMenu(hmenu)

    def _wnd_proc(self, hwnd, msg, wparam, lparam):
        if msg == WM_TRAY:
            if lparam in (WM_RBUTTONUP, WM_CONTEXTMENU, WM_RBUTTONDOWN, WM_LBUTTONUP):
                # 左键单击托盘也弹出菜单，方便触控板用户
                self._show_menu(hwnd)
            elif lparam == WM_LBUTTONDBLCLK:
                if self.on_toggle_text:
                    self.on_toggle_text()
            return 0
        if msg == WM_TRAY + 1:
            self._apply_tip()
            return 0
        if msg == WM_DESTROY:
            self._remove_icon()
            self._PostQuitMessage(0)
            return 0
        return self._DefWindowProcW(hwnd, msg, wparam, lparam)


# ---------------------------------------------------------------------------
# 悬浮球 UI（tkinter）
# ---------------------------------------------------------------------------

class CpuBallApp:
    def __init__(self) -> None:
        import tkinter as tk

        self.tk = tk
        self.root = tk.Tk()
        self.root.title("CPU Ball")
        self.root.overrideredirect(True)
        self.root.attributes("-topmost", True)
        self.root.attributes("-alpha", 0.55)
        # 用黑色作为透明色，画布只画彩色球体
        self.transparent_color = "#000000"
        try:
            self.root.wm_attributes("-transparentcolor", self.transparent_color)
        except tk.TclError:
            pass
        self.root.configure(bg=self.transparent_color)

        # 初始位置：屏幕左下角（工作区，避开任务栏）
        try:
            class _RECT(ctypes.Structure):
                _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                            ("right", ctypes.c_long), ("bottom", ctypes.c_long)]
            wa = _RECT()
            if not user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(wa), 0):
                raise OSError("SPI_GETWORKAREA failed")
            x = wa.left + 24
            y = wa.bottom - BALL_SIZE - 24
        except Exception:
            sh = self.root.winfo_screenheight()
            x = 24
            y = max(20, sh - BALL_SIZE - 80)
        self.root.geometry(f"{BALL_SIZE}x{BALL_SIZE}+{x}+{y}")

        self.canvas = tk.Canvas(
            self.root,
            width=BALL_SIZE,
            height=BALL_SIZE,
            bg=self.transparent_color,
            highlightthickness=0,
            bd=0,
        )
        self.canvas.pack(fill="both", expand=True)

        self.sampler = CpuSampler()
        self.smoothed = 0.0
        self.always_on_top = True
        self.show_text = True
        self._drag_data = {"x": 0, "y": 0}
        self._running = True
        self._tray: Optional[TrayIcon] = None
        self._paint_static()

        # 交互
        self.canvas.bind("<Button-1>", self._on_drag_start)
        self.canvas.bind("<B1-Motion>", self._on_drag_move)
        self.canvas.bind("<ButtonRelease-1>", self._on_drag_stop)
        self.canvas.bind("<Button-3>", self._on_ball_menu)
        self.root.bind("<Double-Button-1>", lambda e: self.toggle_text())
        self.root.protocol("WM_DELETE_WINDOW", self.quit_app)

        # 托盘
        self._tray = TrayIcon(
            on_exit=self._request_exit,
            on_toggle_top=self.toggle_topmost,
            on_toggle_text=self.toggle_text,
            tooltip_base="CPU 负载球",
        )
        self._tray.start()

        # 启动采样循环
        self.root.after(UPDATE_MS, self._tick)

    # ----- 绘制 -----

    def _paint_static(self) -> None:
        c = self.canvas
        c.delete("all")
        # 预留各层 id，动态更新
        self._id_glow = c.create_oval(2, 2, BALL_SIZE - 2, BALL_SIZE - 2, fill="", outline="", width=2)
        self._id_outer = c.create_oval(8, 8, BALL_SIZE - 8, BALL_SIZE - 8, fill="#2ecc71", outline="")
        self._id_inner = c.create_oval(20, 20, BALL_SIZE - 20, BALL_SIZE - 20, fill="#27ae60", outline="")
        self._id_spec = c.create_oval(
            22, 18, 22 + int(BALL_SIZE * 0.28), 18 + int(BALL_SIZE * 0.18),
            fill="#ffffff", outline="",  # 透明度用 stipple 近似
        )
        c.itemconfigure(self._id_spec, fill="#000000", stipple="gray25")
        # 高光改用半透明白：直接用浅色块
        c.coords(
            self._id_spec,
            BALL_SIZE * 0.22, BALL_SIZE * 0.16,
            BALL_SIZE * 0.52, BALL_SIZE * 0.34,
        )
        c.itemconfigure(self._id_spec, fill="#ffffff", stipple="gray50", outline="")

        self._id_text = c.create_text(
            BALL_SIZE // 2,
            BALL_SIZE // 2,
            text="0%",
            fill="#ecf0f1",
            font=(FONT_FAMILY, max(18, BALL_SIZE // 4), "bold"),
        )
        self._id_ring = c.create_oval(6, 6, BALL_SIZE - 6, BALL_SIZE - 6, fill="", outline="#ffffff", width=2)

    def _apply_visual(self, pct: float) -> None:
        base = load_color(pct)
        outer = blend_color(base, "#000000", 0.15)
        inner = blend_color(base, "#ffffff", 0.25)
        glow = blend_color(base, "#ffffff", 0.55)
        # 轻载时环更暗，重载时环更亮（脉冲感）
        ring_t = 0.15 + (pct / 100.0) * 0.65
        ring = blend_color(base, "#ffffff", ring_t)

        c = self.canvas
        c.itemconfigure(self._id_glow, outline=glow)
        c.itemconfigure(self._id_outer, fill=outer)
        c.itemconfigure(self._id_inner, fill=inner)
        c.itemconfigure(self._id_ring, outline=ring)
        if self.show_text:
            c.itemconfigure(self._id_text, text=f"{pct:.0f}%", state="normal")
            # 重载时文字加重描边感（深色字在亮球上）
            if pct < 50:
                c.itemconfigure(self._id_text, fill="#1a1a1a")
            else:
                c.itemconfigure(self._id_text, fill="#ffffff")
        else:
            c.itemconfigure(self._id_text, state="hidden")

    # ----- 循环 -----

    def _tick(self) -> None:
        if not self._running:
            return
        try:
            raw = self.sampler.sample()
            self.force_topmost()
            # EMA 平滑，避免数字抖动
            if self.smoothed == 0.0:
                self.smoothed = raw
            else:
                self.smoothed = EMA_ALPHA * raw + (1 - EMA_ALPHA) * self.smoothed
            self._apply_visual(self.smoothed)
            if self._tray:
                self._tray.update_tip(self.smoothed)
        except Exception:
            traceback.print_exc()
        if self._running:
            self.root.after(UPDATE_MS, self._tick)

    # ----- 交互 -----

    def _on_drag_start(self, event) -> None:
        self._drag_data["x"] = event.x
        self._drag_data["y"] = event.y

    def _on_drag_move(self, event) -> None:
        dx = event.x - self._drag_data["x"]
        dy = event.y - self._drag_data["y"]
        x = self.root.winfo_x() + dx
        y = self.root.winfo_y() + dy
        self.root.geometry(f"+{x}+{y}")

    def _on_drag_stop(self, _event) -> None:
        pass

    def _on_ball_menu(self, event) -> None:
        """球上右键：也提供退出菜单（托盘不可用时的兜底）。"""
        menu = self.tk.Menu(self.root, tearoff=0)
        menu.add_command(label="显示/隐藏百分比", command=self.toggle_text)
        menu.add_command(label="始终置顶（已启用）", state="disabled")
        menu.add_separator()
        menu.add_command(label="退出 CPU 球", command=self.quit_app)
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()

    def force_topmost(self) -> None:
        """仅在「始终置顶」打开时保持置顶。"""
        if not getattr(self, "always_on_top", True):
            return
        try:
            self.root.attributes("-topmost", True)
        except self.tk.TclError:
            return
        try:
            user32.SetWindowPos(
                self.root.winfo_id(), HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW,
            )
        except Exception:
            pass

    def toggle_topmost(self) -> None:
        """托盘右键可开关「始终置顶」。"""
        self.always_on_top = not getattr(self, "always_on_top", True)
        try:
            self.root.attributes("-topmost", bool(self.always_on_top))
        except self.tk.TclError:
            return
        try:
            insert = HWND_TOPMOST if self.always_on_top else HWND_NOTOPMOST
            user32.SetWindowPos(
                self.root.winfo_id(), insert, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW,
            )
        except Exception:
            pass
    def toggle_text(self) -> None:
        self.show_text = not self.show_text
        self._apply_visual(self.smoothed)

    def _request_exit(self) -> None:
        # 可能从托盘线程调用，调度回主线程
        try:
            self.root.after(0, self.quit_app)
        except Exception:
            self.quit_app()

    def quit_app(self) -> None:
        if not self._running:
            return
        self._running = False
        if self._tray:
            try:
                self._tray.stop()
            except Exception:
                pass
        try:
            self.root.destroy()
        except Exception:
            pass
        sys.exit(0)

    def run(self) -> None:
        self.root.mainloop()


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------

def main() -> None:
    if sys.platform != "win32":
        print("当前脚本面向 Windows（GetSystemTimes + 系统托盘）。")
        sys.exit(1)
    # DPI 感知，避免模糊
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass

    app = CpuBallApp()
    app.run()


if __name__ == "__main__":
    main()

