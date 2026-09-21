# -*- coding: utf-8 -*-
"""
WakeGuard — 针对 KB5121003 (26200.9168) 导致的「误报 AC 断开」回归问题。

问题本质：系统每隔一段时间错误地把 AC 判定为断开 1~5 秒，Modern Standby 机器
(S0ix) 随即按 DC 侧策略息屏并滑入连接待机（Kernel-Power 506），随后又退出（507）。

对策：用户态无法否决已进入的睡眠转换，因此采取「预防式」——
  1. 持续持有 PowerRequest (DisplayRequired + SystemRequired)，让系统根本不走到息屏；
  2. AC 去抖（sticky）：只要最近 N 秒内出现过 AC 在线就判定为物理接电，1~5 秒误报被完全吸收；
  3. 尊重用户主动操作：锁屏 / 合盖 / 菜单放行时自动释放请求。

环境：Python 3 + pystray + Pillow（无需管理员权限）
"""

import ctypes
import logging
import os
import subprocess
import sys
import threading
import time
import winreg
from ctypes import wintypes as wt
from datetime import datetime
from pathlib import Path

import pystray
from PIL import Image, ImageDraw

APP_NAME = "WakeGuard"
LOG_DIR = Path(os.environ.get("LOCALAPPDATA", ".")) / APP_NAME
LOG_DIR.mkdir(parents=True, exist_ok=True)
LOG_FILE = LOG_DIR / "wakeguard.log"
SCRIPT_PATH = str(Path(__file__).resolve())
PYTHONW = str(Path(sys.executable).with_name("pythonw.exe"))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[logging.FileHandler(LOG_FILE, encoding="utf-8")],
)
log = logging.getLogger(APP_NAME)

# ---------------------------------------------------------------------------
# Win32 声明
# ---------------------------------------------------------------------------
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)
wtsapi32 = ctypes.WinDLL("wtsapi32", use_last_error=True)

ES_CONTINUOUS = 0x80000000
ES_SYSTEM_REQUIRED = 0x00000001
ES_DISPLAY_REQUIRED = 0x00000002

PowerRequestDisplayRequired = 0
PowerRequestSystemRequired = 1

POWER_REQUEST_CONTEXT_VERSION = 0
POWER_REQUEST_CONTEXT_SIMPLE_STRING = 0x00000001


class SYSTEM_POWER_STATUS(ctypes.Structure):
    _fields_ = [
        ("ACLineStatus", ctypes.c_ubyte),
        ("BatteryFlag", ctypes.c_ubyte),
        ("BatteryLifePercent", ctypes.c_ubyte),
        ("SystemStatusFlag", ctypes.c_ubyte),
        ("BatteryLifeTime", wt.DWORD),
        ("BatteryFullLifeTime", wt.DWORD),
    ]


class _DETAILED(ctypes.Structure):
    _fields_ = [
        ("LocalizedReasonModule", wt.HMODULE),
        ("LocalizedReasonId", ctypes.c_ulong),
        ("ReasonStringCount", ctypes.c_ulong),
        ("ReasonStrings", ctypes.POINTER(wt.LPWSTR)),
    ]


class _REASON(ctypes.Union):
    _fields_ = [("Detailed", _DETAILED), ("SimpleReasonString", wt.LPWSTR)]


class REASON_CONTEXT(ctypes.Structure):
    _fields_ = [
        ("Version", ctypes.c_ulong),
        ("Flags", wt.DWORD),
        ("Reason", _REASON),
    ]


kernel32.PowerCreateRequest.restype = wt.HANDLE
kernel32.PowerCreateRequest.argtypes = [ctypes.POINTER(REASON_CONTEXT)]
kernel32.PowerSetRequest.restype = wt.BOOL
kernel32.PowerSetRequest.argtypes = [wt.HANDLE, ctypes.c_int]
kernel32.PowerClearRequest.restype = wt.BOOL
kernel32.PowerClearRequest.argtypes = [wt.HANDLE, ctypes.c_int]
kernel32.GetSystemPowerStatus.argtypes = [ctypes.POINTER(SYSTEM_POWER_STATUS)]
kernel32.GetSystemPowerStatus.restype = wt.BOOL
kernel32.SetThreadExecutionState.restype = wt.DWORD
kernel32.SetThreadExecutionState.argtypes = [wt.DWORD]
kernel32.CloseHandle.restype = wt.BOOL
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.GetModuleHandleW.restype = wt.HMODULE
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
kernel32.CreateMutexW.restype = wt.HANDLE
kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.LPCWSTR]
kernel32.GetLastError.restype = wt.DWORD

WM_POWERBROADCAST = 0x0218
WM_WTSSESSION_CHANGE = 0x02B1
WM_DESTROY = 0x0002
WM_QUIT = 0x0012
PBT_APMSUSPEND = 0x0004
PBT_APMRESUMEAUTOMATIC = 0x0012
PBT_APMRESUMESUSPEND = 0x0007
PBT_POWERSETTINGCHANGE = 0x8013
WTS_SESSION_LOCK = 0x7
WTS_SESSION_UNLOCK = 0x8
NOTIFY_FOR_THIS_SESSION = 0x00000000
DEVICE_NOTIFY_WINDOW_HANDLE = 0x00000000

GUID_ACDC_POWER_SOURCE = "5D3E9A59-E9D5-4B00-A6BD-FF34FF516548"
GUID_MONITOR_POWER_ON = "02731015-4510-4526-99E6-E5A17EBD1EA6"
GUID_LIDSWITCH_STATE_CHANGE = "BA3E0F4D-B817-4094-A2A1-D56386E4AE7D"


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_ulong),
        ("Data2", ctypes.c_ushort),
        ("Data3", ctypes.c_ushort),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class POWERBROADCAST_SETTING(ctypes.Structure):
    _fields_ = [
        ("PowerSetting", GUID),
        ("DataLength", wt.DWORD),
        ("Data", ctypes.c_ubyte * 1),
    ]


def make_guid(text: str) -> GUID:
    g = GUID()
    p = text.split("-")
    g.Data1 = int(p[0], 16)
    g.Data2 = int(p[1], 16)
    g.Data3 = int(p[2], 16)
    blob = bytes.fromhex(p[3] + p[4])
    g.Data4 = (ctypes.c_ubyte * 8)(*blob)
    return g


def guid_text(g: GUID) -> str:
    d4 = bytes(g.Data4)
    return (
        f"{g.Data1:08X}-{g.Data2:04X}-{g.Data3:04X}-"
        f"{d4[:2].hex().upper()}-{d4[2:].hex().upper()}"
    )


user32.RegisterPowerSettingNotification.restype = wt.HANDLE
user32.RegisterPowerSettingNotification.argtypes = [wt.HANDLE, ctypes.POINTER(GUID), wt.DWORD]
user32.UnregisterPowerSettingNotification.argtypes = [wt.HANDLE]
user32.UnregisterPowerSettingNotification.restype = wt.BOOL
wtsapi32.WTSRegisterSessionNotification.argtypes = [wt.HWND, wt.DWORD]
wtsapi32.WTSRegisterSessionNotification.restype = wt.BOOL
wtsapi32.WTSUnRegisterSessionNotification.argtypes = [wt.HWND]

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.UINT),
        ("style", wt.UINT),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wt.HINSTANCE),
        ("hIcon", wt.HICON),
        ("hCursor", wt.HANDLE),
        ("hbrBackground", wt.HBRUSH),
        ("lpszMenuName", wt.LPCWSTR),
        ("lpszClassName", wt.LPCWSTR),
        ("hIconSm", wt.HICON),
    ]


user32.DefWindowProcW.restype = LRESULT
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.RegisterClassExW.argtypes = [ctypes.POINTER(WNDCLASSEXW)]
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [
    wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID,
]
user32.DestroyWindow.restype = wt.BOOL
user32.DestroyWindow.argtypes = [wt.HWND]
user32.GetMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT]
user32.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
user32.PostQuitMessage.argtypes = [ctypes.c_int]

# ---------------------------------------------------------------------------
# 电源请求封装
# ---------------------------------------------------------------------------

REASON_TEXT = "WakeGuard: 防止 KB5121003 误报 AC 断开导致的息屏与连接待机"


class PowerRequest:
    """持有 DisplayRequired + SystemRequired，阻止息屏与进入 Modern Standby。"""

    def __init__(self):
        self._handle = None

    def _ensure_handle(self) -> bool:
        if self._handle:
            return True
        ctx = REASON_CONTEXT()
        ctx.Version = POWER_REQUEST_CONTEXT_VERSION
        ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING
        ctx.Reason.SimpleReasonString = REASON_TEXT
        h = kernel32.PowerCreateRequest(ctypes.byref(ctx))
        if not h:
            log.error("PowerCreateRequest 失败: %s", ctypes.get_last_error())
            return False
        self._handle = h
        return True

    def acquire(self) -> bool:
        if not self._ensure_handle():
            return False
        ok = True
        for req in (PowerRequestDisplayRequired, PowerRequestSystemRequired):
            if not kernel32.PowerSetRequest(self._handle, req):
                log.error("PowerSetRequest(%d) 失败: %s", req, ctypes.get_last_error())
                ok = False
        kernel32.SetThreadExecutionState(
            ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED
        )
        return ok

    def release(self) -> None:
        if self._handle:
            for req in (PowerRequestDisplayRequired, PowerRequestSystemRequired):
                kernel32.PowerClearRequest(self._handle, req)
        kernel32.SetThreadExecutionState(ES_CONTINUOUS)

    def close(self) -> None:
        self.release()
        if self._handle:
            kernel32.CloseHandle(self._handle)
            self._handle = None


def get_power_status():
    st = SYSTEM_POWER_STATUS()
    if not kernel32.GetSystemPowerStatus(ctypes.byref(st)):
        return None
    return st


# ---------------------------------------------------------------------------
# 守护核心
# ---------------------------------------------------------------------------


class WakeGuard:
    AC_STICKY_SECONDS = 60.0      # 拔电后维持保护的宽容窗口
    POLL_INTERVAL = 0.5
    MANUAL_ALLOW_TIMEOUT = 300.0  # 「允许下一次睡眠」最长等待

    def __init__(self):
        self._req = PowerRequest()
        self._lock = threading.RLock()
        self.holding = False
        self.last_ac_seen = 0.0
        self._prev_online = None
        self.paused_until = 0.0
        self.allow_once_until = 0.0
        self.lid_closed = False
        self.session_locked = False
        self.ac_online = False
        self.sticky_ac = False
        self.battery_percent = None
        self.stats = {
            "ac_glitch": 0,       # AC 掉电跳变次数（bug 触发次数）
            "monitor_off": 0,     # 显示器被关闭次数
            "suspend": 0,         # 系统进入待机次数
            "hold_seconds": 0.0,
            "started": datetime.now(),
        }
        self._hold_since = None
        self._stop = threading.Event()

    # -- 状态判定 ---------------------------------------------------------
    def _blocked_reason(self):
        now = time.time()
        if self.lid_closed:
            return "合盖"
        if self.session_locked:
            return "已锁屏"
        if now < self.paused_until:
            return f"已暂停（剩余 {int(self.paused_until - now)} 秒）"
        if now < self.allow_once_until:
            return "已放行本次睡眠"
        return None

    def is_blocked(self) -> bool:
        return self._blocked_reason() is not None

    def status_line(self) -> str:
        with self._lock:
            reason = self._blocked_reason()
            if reason:
                return f"未保护 · {reason}"
            if self.holding:
                pct = f"{self.battery_percent}%" if self.battery_percent is not None else "-"
                return f"保护中 · 电源接入 · 电量 {pct}"
            if self.ac_online:
                return "准备接管…"
            return "未接电 · 已交还系统策略"

    def stats_line(self) -> str:
        s = self.stats
        return (
            f"误报掉电 {s['ac_glitch']} 次 · 息屏 {s['monitor_off']} 次 · "
            f"待机 {s['suspend']} 次"
        )

    # -- 请求控制 ---------------------------------------------------------
    def _apply(self) -> None:
        now = time.time()
        with self._lock:
            self.sticky_ac = (now - self.last_ac_seen) < self.AC_STICKY_SECONDS
            want = self.sticky_ac and not self.is_blocked()
            if want and not self.holding:
                if self._req.acquire():
                    self.holding = True
                    self._hold_since = now
                    log.info("已接管：持有显示 + 系统电源请求")
                else:
                    log.error("接管失败")
            elif not want and self.holding:
                self._req.release()
                self.holding = False
                if self._hold_since:
                    self.stats["hold_seconds"] += now - self._hold_since
                    self._hold_since = None
                log.info("已释放电源请求（%s）", self._blocked_reason() or "不再满足接电条件")

    # -- 轮询线程 ---------------------------------------------------------
    def loop(self):
        while not self._stop.wait(self.POLL_INTERVAL):
            st = get_power_status()
            if st is None:
                continue
            online = st.ACLineStatus == 1
            with self._lock:
                self.ac_online = online
                self.battery_percent = (
                    int(st.BatteryLifePercent) if st.BatteryLifePercent <= 100 else None
                )
                if online:
                    self.last_ac_seen = time.time()
                    if self._prev_online is False:
                        self.stats["ac_glitch"] += 1
                        log.warning("检测到 AC 恢复（此前被误报断开）—— 第 %d 次跳变",
                                    self.stats["ac_glitch"])
                else:
                    if self._prev_online is True:
                        log.warning("检测到 AC 断开（sticky 窗口内继续保护）")
                self._prev_online = online
            self._apply()

    def stop(self):
        self._stop.set()
        if self.holding:
            self._req.release()
            self.holding = False
        self._req.close()

    # -- 外部动作 ---------------------------------------------------------
    def pause(self, seconds: float):
        with self._lock:
            self.paused_until = time.time() + seconds
        log.info("暂停保护 %d 秒", seconds)
        self._apply()

    def resume(self):
        with self._lock:
            self.paused_until = 0.0
            self.allow_once_until = 0.0
        log.info("恢复保护")
        self._apply()

    def allow_sleep_once(self):
        with self._lock:
            self.allow_once_until = time.time() + self.MANUAL_ALLOW_TIMEOUT
        log.info("放行下一次睡眠（最长等待 %d 秒）", int(self.MANUAL_ALLOW_TIMEOUT))
        self._apply()

    # -- 通知回调 ---------------------------------------------------------
    def on_power_setting(self, guid_str: str, data: bytes):
        if not data:
            return
        value = data[0]
        if guid_str == GUID_ACDC_POWER_SOURCE:
            # 0=电池 1=AC 2=未知
            if value == 1:
                with self._lock:
                    self.last_ac_seen = time.time()
        elif guid_str == GUID_MONITOR_POWER_ON:
            if value == 0:
                self.stats["monitor_off"] += 1
                log.warning("显示器已关闭（第 %d 次）holding=%s",
                            self.stats["monitor_off"], self.holding)
        elif guid_str == GUID_LIDSWITCH_STATE_CHANGE:
            with self._lock:
                self.lid_closed = value == 0
            log.info("盖子状态：%s", "关闭" if self.lid_closed else "打开")
        self._apply()

    def on_session(self, wparam: int):
        if wparam == WTS_SESSION_LOCK:
            with self._lock:
                self.session_locked = True
            log.info("会话已锁定，交还电源策略")
        elif wparam == WTS_SESSION_UNLOCK:
            with self._lock:
                self.session_locked = False
            log.info("会话已解锁")
        else:
            return
        self._apply()

    def on_suspend(self):
        self.stats["suspend"] += 1
        log.warning("系统进入待机（第 %d 次）", self.stats["suspend"])

    def on_resume(self):
        log.info("系统已恢复")
        with self._lock:
            self.allow_once_until = 0.0
        self._apply()


# ---------------------------------------------------------------------------
# 隐藏消息窗口（接收电源 / 会话通知）
# ---------------------------------------------------------------------------


class HiddenWindow(threading.Thread):
    def __init__(self, guard: WakeGuard):
        super().__init__(daemon=True, name="WakeGuardWindow")
        self.guard = guard
        self.hwnd = None
        self._notifies = []
        self._proc = None

    def run(self):
        self._proc = WNDPROC(self._wndproc)
        wc = WNDCLASSEXW()
        wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
        wc.lpfnWndProc = self._proc
        wc.lpszClassName = "WakeGuardMsgWindow"
        wc.hInstance = kernel32.GetModuleHandleW(None)
        if not user32.RegisterClassExW(ctypes.byref(wc)):
            log.error("RegisterClassExW 失败: %s", ctypes.get_last_error())
            return

        hwnd = user32.CreateWindowExW(
            0, "WakeGuardMsgWindow", "WakeGuard", 0,
            0, 0, 0, 0, None, None, wc.hInstance, None,
        )
        if not hwnd:
            log.error("CreateWindowExW 失败: %s", ctypes.get_last_error())
            return
        self.hwnd = hwnd
        log.info("隐藏窗口已创建 hwnd=%s", hwnd)

        for g in (GUID_ACDC_POWER_SOURCE, GUID_MONITOR_POWER_ON, GUID_LIDSWITCH_STATE_CHANGE):
            guid = make_guid(g)
            h = user32.RegisterPowerSettingNotification(
                hwnd, ctypes.byref(guid), DEVICE_NOTIFY_WINDOW_HANDLE
            )
            if h:
                self._notifies.append(h)
            else:
                log.error("注册电源通知失败 %s: %s", g, ctypes.get_last_error())

        if not wtsapi32.WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION):
            log.error("WTSRegisterSessionNotification 失败: %s", ctypes.get_last_error())

        msg = wt.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

    def _wndproc(self, hwnd, umsg, wparam, lparam):
        if umsg == WM_POWERBROADCAST:
            if wparam == PBT_POWERSETTINGCHANGE:
                try:
                    s = ctypes.cast(lparam, ctypes.POINTER(POWERBROADCAST_SETTING)).contents
                    data = bytes(ctypes.string_at(
                        ctypes.byref(s, POWERBROADCAST_SETTING.Data.offset),
                        s.DataLength))
                    self.guard.on_power_setting(guid_text(s.PowerSetting), data)
                except Exception as exc:  # noqa: BLE001
                    log.error("解析电源设置通知失败: %s", exc)
            elif wparam == PBT_APMSUSPEND:
                self.guard.on_suspend()
            elif wparam in (PBT_APMRESUMEAUTOMATIC, PBT_APMRESUMESUSPEND):
                self.guard.on_resume()
            return 1
        if umsg == WM_WTSSESSION_CHANGE:
            self.guard.on_session(wparam)
            return 0
        if umsg == WM_DESTROY:
            user32.PostQuitMessage(0)
            return 0
        return user32.DefWindowProcW(hwnd, umsg, wparam, lparam)

    def close(self):
        for h in self._notifies:
            user32.UnregisterPowerSettingNotification(h)
        self._notifies.clear()
        if self.hwnd:
            wtsapi32.WTSUnRegisterSessionNotification(self.hwnd)
            user32.DestroyWindow(self.hwnd)
            self.hwnd = None


# ---------------------------------------------------------------------------
# 托盘图标
# ---------------------------------------------------------------------------


def make_icon(kind: str = "active") -> Image.Image:
    colors = {
        "active": ("#1D9E75", "#E1F5EE"),
        "idle": ("#888780", "#F1EFE8"),
        "paused": ("#EF9F27", "#FAEEDA"),
    }
    fill, bolt = colors.get(kind, colors["idle"])
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2, 2, 61, 61], radius=16, fill=fill)
    d.polygon([(36, 10), (20, 34), (31, 34), (27, 54), (45, 28), (33, 28)], fill=bolt)
    return img


def is_autostart_enabled() -> bool:
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Run",
        ) as key:
            value, _ = winreg.QueryValueEx(key, APP_NAME)
            return SCRIPT_PATH in value
    except OSError:
        return False


def is_screensaver_enabled() -> bool:
    """屏保不受电源请求约束 —— 开着就可能照样黑屏。"""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Control Panel\\Desktop") as key:
            value, _ = winreg.QueryValueEx(key, "ScreenSaveActive")
            return str(value) == "1"
    except OSError:
        return False


def set_screensaver(enabled: bool) -> bool:
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, "Control Panel\\Desktop", 0, winreg.KEY_SET_VALUE
        ) as key:
            winreg.SetValueEx(
                key, "ScreenSaveActive", 0, winreg.REG_SZ, "1" if enabled else "0"
            )
        return True
    except OSError as exc:
        log.error("设置屏幕保护失败: %s", exc)
        return False


def set_autostart(enabled: bool) -> bool:
    path = r"Software\Microsoft\Windows\CurrentVersion\Run"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_SET_VALUE) as key:
            if enabled:
                winreg.SetValueEx(
                    key, APP_NAME, 0, winreg.REG_SZ, f'"{PYTHONW}" "{SCRIPT_PATH}"'
                )
            else:
                try:
                    winreg.DeleteValue(key, APP_NAME)
                except OSError:
                    pass
        return True
    except OSError as exc:
        log.error("设置自启动失败: %s", exc)
        return False


# ---------------------------------------------------------------------------
# 主入口
# ---------------------------------------------------------------------------


_MUTEX = None


def acquire_single_instance() -> bool:
    """进程生命周期内持有命名互斥量句柄，保证只有一个实例。"""
    global _MUTEX
    handle = kernel32.CreateMutexW(None, True, "Local\\WakeGuard_Singleton_Mutex")
    if not handle:
        log.error("CreateMutexW 失败: %s", kernel32.GetLastError())
        return True
    if kernel32.GetLastError() == 183:  # ERROR_ALREADY_EXISTS
        kernel32.CloseHandle(handle)
        return False
    _MUTEX = handle
    return True


def selftest(seconds: int) -> int:
    """无托盘的自检：跑 N 秒，验证电源请求能否建立，然后退出。"""
    print(f"[selftest] 运行 {seconds} 秒，观察电源请求与 AC 状态")
    guard = WakeGuard()
    win = HiddenWindow(guard)
    win.start()
    threading.Thread(target=guard.loop, daemon=True, name="WakeGuardLoop").start()

    st = get_power_status()
    print(f"[selftest] 初始 AC = {st.ACLineStatus} "
          f"({ {0: '电池', 1: '交流', 255: '未知'}.get(st.ACLineStatus) })")
    deadline = time.time() + seconds
    while time.time() < deadline:
        time.sleep(1)
        print(f"[selftest] holding={guard.holding} ac={guard.ac_online} "
              f"sticky={guard.sticky_ac} {guard.status_line()}")
    print(f"[selftest] 统计: {guard.stats_line()}")
    guard.stop()
    win.close()
    print("[selftest] 完成，详见", LOG_FILE)
    return 0


def main():
    # 单实例
    if not acquire_single_instance():
        log.warning("WakeGuard 已在运行，本次退出")
        return 1

    log.info("=" * 60)
    log.info("WakeGuard 启动 | Python %s", sys.version.split()[0])
    st = get_power_status()
    log.info("初始 AC 状态: %s",
             {0: "电池", 1: "交流", 255: "未知"}.get(st.ACLineStatus if st else 255))
    if is_screensaver_enabled():
        log.warning("屏幕保护处于启用状态；它不受电源请求约束，可能是另一个息屏来源")

    guard = WakeGuard()
    win = HiddenWindow(guard)
    win.start()
    threading.Thread(target=guard.loop, daemon=True, name="WakeGuardLoop").start()

    icon = None

    def state_kind():
        if guard.holding:
            return "active"
        if guard.is_blocked():
            return "paused"
        return "idle"

    def on_toggle_pause(icon_, item):
        if guard.paused_until > time.time() or guard.is_blocked():
            guard.resume()
        else:
            guard.pause(30 * 60)

    def on_allow_once(icon_, item):
        guard.allow_sleep_once()

    def on_autostart(icon_, item):
        set_autostart(not is_autostart_enabled())

    def on_screensaver(icon_, item):
        set_screensaver(not is_screensaver_enabled())

    def on_open_log(icon_, item):
        subprocess.Popen(["explorer", str(LOG_DIR)])

    def on_exit(icon_, item):
        log.info("用户退出，运行 %.1f 分钟，累计保护 %.1f 分钟",
                 (datetime.now() - guard.stats["started"]).total_seconds() / 60,
                 guard.stats["hold_seconds"] / 60)
        icon_.stop()

    def menu():
        paused = guard.is_blocked()
        return pystray.Menu(
            pystray.MenuItem(lambda _: guard.status_line(), None, enabled=False),
            pystray.MenuItem(lambda _: guard.stats_line(), None, enabled=False),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem(
                "恢复保护" if paused else "暂停保护 30 分钟", on_toggle_pause
            ),
            pystray.MenuItem(
                "放行下一次睡眠",
                on_allow_once,
                checked=lambda _: guard.allow_once_until > time.time(),
            ),
            pystray.MenuItem(
                "允许屏幕保护（不建议）",
                on_screensaver,
                checked=lambda _: is_screensaver_enabled(),
            ),
            pystray.MenuItem(
                "开机自启动", on_autostart, checked=lambda _: is_autostart_enabled()
            ),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("打开日志目录", on_open_log),
            pystray.MenuItem("退出", on_exit),
        )

    icon = pystray.Icon(APP_NAME, make_icon("idle"), APP_NAME, menu=menu())
    icon.menu = menu()

    def refresher():
        last = None
        while True:
            try:
                kind = state_kind()
                if kind != last:
                    icon.icon = make_icon(kind)
                    last = kind
                icon.update_menu()
                if not icon.visible:
                    break
            except Exception as exc:  # noqa: BLE001
                log.error("刷新托盘失败: %s", exc)
            time.sleep(2)

    threading.Thread(target=refresher, daemon=True, name="WakeGuardUI").start()
    log.info("托盘已就绪")
    icon.run()

    guard.stop()
    win.close()
    return 0


if __name__ == "__main__":
    try:
        if "--selftest" in sys.argv:
            idx = sys.argv.index("--selftest")
            secs = int(sys.argv[idx + 1]) if len(sys.argv) > idx + 1 else 10
            sys.exit(selftest(secs))
        sys.exit(main())
    except Exception:  # noqa: BLE001
        log.exception("未捕获异常导致退出")
        raise
