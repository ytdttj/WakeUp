using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace WakeGuard;

// ---------------------------------------------------------------------------
// Win32
// ---------------------------------------------------------------------------

[StructLayout(LayoutKind.Sequential)]
internal struct SYSTEM_POWER_STATUS
{
    public byte ACLineStatus;
    public byte BatteryFlag;
    public byte BatteryLifePercent;
    public byte SystemStatusFlag;
    public uint BatteryLifeTime;
    public uint BatteryFullLifeTime;
}

[StructLayout(LayoutKind.Sequential, Size = 32)]
internal struct REASON_CONTEXT
{
    public uint Version;
    public uint Flags;
    public IntPtr SimpleReasonString;
}

[StructLayout(LayoutKind.Sequential)]
internal struct POWERBROADCAST_SETTING
{
    public Guid PowerSetting;
    public uint DataLength;
    public byte Data;
}

[StructLayout(LayoutKind.Sequential)]
internal struct POINT { public int X; public int Y; }

[StructLayout(LayoutKind.Sequential)]
internal struct MSG
{
    public IntPtr hwnd; public uint message; public IntPtr wParam;
    public IntPtr lParam; public uint time; public POINT pt; public uint lPrivate;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct WNDCLASSEXW
{
    public uint cbSize; public uint style; public IntPtr lpfnWndProc;
    public int cbClsExtra; public int cbWndExtra; public IntPtr hInstance;
    public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground;
    public string lpszMenuName; public string lpszClassName; public IntPtr hIconSm;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct NOTIFYICONDATAW
{
    public uint cbSize; public IntPtr hWnd; public uint uID; public uint uFlags;
    public uint uCallbackMessage; public IntPtr hIcon;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
    public uint dwState; public uint dwStateMask;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
    public uint uVersion;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
    public uint dwInfoFlags; public Guid guidItem; public IntPtr hBalloonIcon;
}

[StructLayout(LayoutKind.Sequential)]
internal struct BITMAPINFOHEADER
{
    public uint biSize; public int biWidth; public int biHeight;
    public ushort biPlanes; public ushort biBitCount; public uint biCompression;
    public uint biSizeImage; public int biXPelsPerMeter; public int biYPelsPerMeter;
    public uint biClrUsed; public uint biClrImportant;
}

[StructLayout(LayoutKind.Sequential)]
internal struct BITMAPINFO { public BITMAPINFOHEADER bmiHeader; public uint bmiColors; }

[StructLayout(LayoutKind.Sequential)]
internal struct ICONINFO
{
    [MarshalAs(UnmanagedType.Bool)] public bool fIcon;
    public uint xHotspot; public uint yHotspot;
    public IntPtr hbmMask; public IntPtr hbmColor;
}

internal static class Native
{
    public const uint ES_CONTINUOUS = 0x80000000;
    public const uint ES_SYSTEM_REQUIRED = 0x00000001;
    public const uint ES_DISPLAY_REQUIRED = 0x00000002;
    public const int PowerRequestDisplayRequired = 0;
    public const int PowerRequestSystemRequired = 1;

    public const uint WM_TIMER = 0x0113;
    public const uint WM_DESTROY = 0x0002;
    public const uint WM_NULL = 0x0000;
    public const uint WM_QUIT = 0x0012;
    public const uint WM_POWERBROADCAST = 0x0218;
    public const uint WM_WTSSESSION_CHANGE = 0x02B1;
    public const uint WM_RBUTTONUP = 0x0205;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint PBT_POWERSETTINGCHANGE = 0x8013;
    public const uint PBT_APMSUSPEND = 0x0004;
    public const uint PBT_APMRESUMEAUTOMATIC = 0x0012;
    public const uint PBT_APMRESUMESUSPEND = 0x0007;
    public const uint WTS_SESSION_LOCK = 0x7;
    public const uint WTS_SESSION_UNLOCK = 0x8;

    public const uint NIM_ADD = 0; public const uint NIM_MODIFY = 1; public const uint NIM_DELETE = 2;
    public const uint NIF_MESSAGE = 1; public const uint NIF_ICON = 2; public const uint NIF_TIP = 4;

    public const uint MF_STRING = 0; public const uint MF_SEPARATOR = 0x800;
    public const uint MF_CHECKED = 8; public const uint MF_GRAYED = 1;
    public const uint TPM_LEFTALIGN = 0x0000; public const uint TPM_RIGHTBUTTON = 0x0002;
    public const uint TPM_RETURNCMD = 0x0100;

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr PowerCreateRequest(ref REASON_CONTEXT ctx);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool PowerSetRequest(IntPtr h, int type);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool PowerClearRequest(IntPtr h, int type);
    [DllImport("kernel32.dll")]
    public static extern bool GetSystemPowerStatus(out SYSTEM_POWER_STATUS st);
    [DllImport("kernel32.dll")]
    public static extern uint SetThreadExecutionState(uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateMutexW(IntPtr a, bool owner, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr GetModuleHandleW(string name);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern ushort RegisterClassExW(ref WNDCLASSEXW wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style,
        int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")]
    public static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool GetMessageW(out MSG msg, IntPtr h, uint min, uint max);
    [DllImport("user32.dll")]
    public static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll")]
    public static extern IntPtr DispatchMessageW(ref MSG msg);
    [DllImport("user32.dll")]
    public static extern void PostQuitMessage(int code);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr RegisterPowerSettingNotification(IntPtr recipient, ref Guid guid, uint flags);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool UnregisterPowerSettingNotification(IntPtr h);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SetTimer(IntPtr h, IntPtr id, uint elapse, IntPtr proc);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool KillTimer(IntPtr h, IntPtr id);

    [DllImport("wtsapi32.dll", SetLastError = true)]
    public static extern bool WTSRegisterSessionNotification(IntPtr h, uint flags);
    [DllImport("wtsapi32.dll", SetLastError = true)]
    public static extern bool WTSUnRegisterSessionNotification(IntPtr h);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool Shell_NotifyIconW(uint msg, ref NOTIFYICONDATAW data);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr CreatePopupMenu();
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool AppendMenuW(IntPtr h, uint flags, IntPtr id, string item);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern int TrackPopupMenu(IntPtr h, uint flags, int x, int y, int res, IntPtr hwnd, IntPtr rect);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool DestroyMenu(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);

    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateCompatibleDC(IntPtr hdc);
    [DllImport("gdi32.dll")]
    public static extern bool DeleteDC(IntPtr hdc);
    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateDIBSection(IntPtr hdc, ref BITMAPINFO bmi, uint usage,
        out IntPtr bits, IntPtr section, uint offset);
    [DllImport("gdi32.dll")]
    public static extern IntPtr SelectObject(IntPtr hdc, IntPtr obj);
    [DllImport("gdi32.dll", SetLastError = true)]
    public static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateSolidBrush(uint color);
    [DllImport("gdi32.dll")]
    public static extern IntPtr CreatePen(int style, int width, uint color);
    [DllImport("gdi32.dll")]
    public static extern IntPtr GetStockObject(int fnObject);
    [DllImport("gdi32.dll", SetLastError = true)]
    public static extern bool RoundRect(IntPtr hdc, int l, int t, int r, int b, int w, int h);
    [DllImport("gdi32.dll", SetLastError = true)]
    public static extern bool Polygon(IntPtr hdc, POINT[] pts, int count);
    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateBitmap(int w, int h, uint planes, uint bits, IntPtr data);
    [DllImport("gdi32.dll", SetLastError = true)]
    public static extern IntPtr CreateIconIndirect(ref ICONINFO info);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool DestroyIcon(IntPtr hIcon);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr CreateIconFromResourceEx(IntPtr bits, uint cbBits,
        [MarshalAs(UnmanagedType.Bool)] bool fIcon, uint version, int cx, int cy, uint flags);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr LoadIconW(IntPtr hInst, IntPtr name);
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

internal static class Log
{
    private static readonly string Dir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WakeGuard");
    private static readonly string File_ = Path.Combine(Dir, "wakeguard.log");
    private static StreamWriter _w;

    public static string Path_ => File_;

    static Log()
    {
        try
        {
            Directory.CreateDirectory(Dir);
            _w = new StreamWriter(new FileStream(File_, FileMode.Append, FileAccess.Write, FileShare.Read), Encoding.UTF8)
            { AutoFlush = true };
        }
        catch { }
    }

    public static void Write(string level, string msg)
    {
        string line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} [{level}] {msg}";
        try { _w?.WriteLine(line); } catch { }
        if (Program.SelfTest) File.AppendAllText(Path.Combine(Dir, "selftest.txt"), line + "\n");
    }

    public static void Info(string m) => Write("INFO", m);
    public static void Warn(string m) => Write("WARN", m);
    public static void Err(string m) => Write("ERROR", m);
}

// ---------------------------------------------------------------------------
// 电源请求
// ---------------------------------------------------------------------------

internal sealed class PowerRequest : IDisposable
{
    private IntPtr _h;
    private readonly IntPtr _reason;

    public bool Held { get; private set; }

    public PowerRequest(string reason)
    {
        _reason = Marshal.StringToHGlobalUni(reason);
    }

    public bool Acquire()
    {
        if (_h == IntPtr.Zero)
        {
            var ctx = new REASON_CONTEXT
            {
                Version = 0,
                Flags = 0x00000001, // POWER_REQUEST_CONTEXT_SIMPLE_STRING
                SimpleReasonString = _reason,
            };
            _h = Native.PowerCreateRequest(ref ctx);
            if (_h == IntPtr.Zero) { Log.Err("PowerCreateRequest 失败"); return false; }
        }
        bool ok = Native.PowerSetRequest(_h, Native.PowerRequestDisplayRequired)
               && Native.PowerSetRequest(_h, Native.PowerRequestSystemRequired);
        Native.SetThreadExecutionState(Native.ES_CONTINUOUS | Native.ES_SYSTEM_REQUIRED
                                       | Native.ES_DISPLAY_REQUIRED);
        Held = ok;
        return ok;
    }

    public void Release()
    {
        if (_h != IntPtr.Zero)
        {
            Native.PowerClearRequest(_h, Native.PowerRequestDisplayRequired);
            Native.PowerClearRequest(_h, Native.PowerRequestSystemRequired);
        }
        Native.SetThreadExecutionState(Native.ES_CONTINUOUS);
        Held = false;
    }

    public void Dispose()
    {
        Release();
        if (_h != IntPtr.Zero) { Native.CloseHandle(_h); _h = IntPtr.Zero; }
        Marshal.FreeHGlobal(_reason);
    }
}

// ---------------------------------------------------------------------------
// 托盘图标（GDI 绘制，无外部资源）
// ---------------------------------------------------------------------------

internal static class TrayIcon
{
    private static readonly (int x, int y)[] Bolt =
        { (19, 4), (10, 18), (15, 18), (13, 29), (22, 12), (16, 12) };

    private static bool InBolt(int x, int y)
    {
        bool inside = false;
        for (int i = 0, j = Bolt.Length - 1; i < Bolt.Length; j = i++)
        {
            int xi = Bolt[i].x, yi = Bolt[i].y, xj = Bolt[j].x, yj = Bolt[j].y;
            if ((yi > y) != (yj > y) && x < (xj - xi) * (double)(y - yi) / (yj - yi) + xi)
                inside = !inside;
        }
        return inside;
    }

    /// <summary>构造一个 32x32 32bpp ICO：实心圆底 + 白色闪电。</summary>
    private static byte[] BuildIco(byte r, byte g, byte b)
    {
        const int W = 32, H = 32;
        byte[] xor = new byte[W * H * 4];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                int i = (y * W + x) * 4;
                double dx = x - 15.5, dy = y - 15.5;
                if (dx * dx + dy * dy > 15.5 * 15.5) continue; // 透明
                bool bolt = InBolt(x, y);
                xor[i] = bolt ? (byte)255 : b;     // B
                xor[i + 1] = bolt ? (byte)255 : g; // G
                xor[i + 2] = bolt ? (byte)255 : r; // R
                xor[i + 3] = 255;                  // A
            }

        // CreateIconFromResourceEx 需要 RT_ICON 资源体（无 ICONDIR 头）
        const int andMask = 128; // 32 位宽、1bpp，行对齐到 4 字节 => 4 * 32
        byte[] res = new byte[40 + xor.Length + andMask];

        Put32(res, 0, 40);     // biSize
        Put32(res, 4, (uint)W);
        Put32(res, 8, H * 2);  // XOR + AND
        res[12] = 1; res[13] = 0;  // biPlanes
        res[14] = 32; res[15] = 0; // biBitCount
        Put32(res, 20, (uint)(xor.Length + andMask)); // biSizeImage
        Array.Copy(xor, 0, res, 40, xor.Length);
        return res;
    }

    private static void Put32(byte[] buf, int off, uint v)
    {
        buf[off] = (byte)(v & 0xFF);
        buf[off + 1] = (byte)((v >> 8) & 0xFF);
        buf[off + 2] = (byte)((v >> 16) & 0xFF);
        buf[off + 3] = (byte)((v >> 24) & 0xFF);
    }

    public static IntPtr Make(byte r, byte g, byte b)
    {
        byte[] ico = BuildIco(r, g, b);
        IntPtr mem = Marshal.AllocHGlobal(ico.Length);
        try
        {
            Marshal.Copy(ico, 0, mem, ico.Length);
            IntPtr h = Native.CreateIconFromResourceEx(mem, (uint)ico.Length, true,
                0x00030000, 0, 0, 0x00008000); // LR_DEFAULTSIZE
            if (h != IntPtr.Zero) return h;
            Log.Err("CreateIconFromResourceEx 失败 err=" + Marshal.GetLastWin32Error());
        }
        finally { Marshal.FreeHGlobal(mem); }
        // 兜底：系统默认图标
        return Native.LoadIconW(IntPtr.Zero, new IntPtr(32512));
    }
}

// ---------------------------------------------------------------------------
// 主程序
// ---------------------------------------------------------------------------

internal static class Program
{
    // 配置
    private const double AcStickySeconds = 60.0;
    private const uint PollMs = 500;
    private const double ManualAllowTimeout = 300.0;

    public static bool SelfTest;
    private static int _selfTestSeconds;

    private static Guid GuidAcDc = new("5d3e9a59-e9d5-4b00-a6bd-ff34ff516548");
    private static Guid GuidMonitor = new("02731015-4510-4526-99e6-e5a17ebd1ea6");
    private static Guid GuidLid = new("ba3e0f4d-b817-4094-a2a1-d56386e4ae7d");

    // 状态
    private static PowerRequest _req;
    private static bool _holding;
    private static double _lastAcSeen;
    private static bool _prevOnline = false;
    private static bool _havePrev;
    private static double _pausedUntil;
    private static double _allowUntil;
    private static bool _lidClosed, _locked;
    private static bool _acOnline, _stickyAc;
    private static int _battery = -1;
    private static int _glitch, _monitorOff, _suspend;
    private static readonly DateTime Start = DateTime.Now;

    private static IntPtr _hwnd;
    private static readonly IntPtr TimerId = new(1);
    private static IntPtr _hIconActive, _hIconIdle, _hIconPaused;
    private static IntPtr _mutex;
    private static readonly List<IntPtr> Notifies = new();
    private static bool _trayAdded;
    private static string _lastTip = "";

    private const uint IdTogglePause = 1001;
    private const uint IdAllowOnce = 1002;
    private const uint IdScreenSaver = 1003;
    private const uint IdAutoStart = 1004;
    private const uint IdOpenLog = 1005;
    private const uint IdExit = 1006;

    private delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    private static readonly WndProc Proc = WindowProc;

    private static double Now => Environment.TickCount64 / 1000.0;

    private static int Main()
    {
        string[] args = Environment.GetCommandLineArgs();
        for (int i = 1; i < args.Length; i++)
        {
            if (args[i] == "--selftest")
            {
                SelfTest = true;
                if (i + 1 < args.Length) int.TryParse(args[i + 1], out _selfTestSeconds);
                if (_selfTestSeconds <= 0) _selfTestSeconds = 10;
            }
            else if (args[i] == "--autostart" && i + 1 < args.Length)
            {
                bool on = args[i + 1] == "on";
                SetAutoStart(on);
                Log.Info($"命令行设置自启动={on} -> {AutoStartEnabled()}");
                return 0;
            }
        }

        _mutex = Native.CreateMutexW(IntPtr.Zero, true, "Local\\WakeGuard_Singleton_Mutex");
        if (_mutex != IntPtr.Zero && Marshal.GetLastWin32Error() == 183)
        {
            Log.Warn("WakeGuard 已在运行，本次退出");
            Native.CloseHandle(_mutex);
            return 1;
        }

        Log.Info("======== WakeGuard (native AOT) 启动 ========");
        var st = default(SYSTEM_POWER_STATUS);
        if (Native.GetSystemPowerStatus(out st))
            Log.Info($"初始 AC 状态: {AcText(st.ACLineStatus)} 电量 {st.BatteryLifePercent}%");
        if (ScreenSaverReallyEnabled())
            Log.Warn("屏幕保护确实启用（有程序且有超时），它不受电源请求约束");

        _req = new PowerRequest("WakeGuard: 防止 KB5121003 误报 AC 断开导致的息屏与连接待机");

        Log.Info("准备生成托盘图标");
        _hIconActive = TrayIcon.Make(0x1D, 0x9E, 0x75); // 绿
        _hIconIdle = TrayIcon.Make(0x88, 0x87, 0x80);   // 灰
        _hIconPaused = TrayIcon.Make(0xEF, 0x9F, 0x27); // 橙
        Log.Info($"图标完成 active={_hIconActive} idle={_hIconIdle} paused={_hIconPaused}");

        IntPtr hInst = Native.GetModuleHandleW(null);
        var wc = new WNDCLASSEXW
        {
            cbSize = (uint)Marshal.SizeOf<WNDCLASSEXW>(),
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(Proc),
            lpszClassName = "WakeGuardMsgWindow",
            hInstance = hInst,
        };
        ushort atom = Native.RegisterClassExW(ref wc);
        Log.Info("窗口类注册 atom=" + atom);
        _hwnd = Native.CreateWindowExW(0, "WakeGuardMsgWindow", "WakeGuard", 0,
            0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, hInst, IntPtr.Zero);
        if (_hwnd == IntPtr.Zero)
        {
            Log.Err("创建窗口失败 err=" + Marshal.GetLastWin32Error());
            return 2;
        }
        Log.Info("隐藏窗口已创建");

        RegisterNotify(ref GuidAcDc);
        RegisterNotify(ref GuidMonitor);
        RegisterNotify(ref GuidLid);
        if (!Native.WTSRegisterSessionNotification(_hwnd, 0))
            Log.Warn("WTSRegisterSessionNotification 失败");

        Native.SetTimer(_hwnd, TimerId, PollMs, IntPtr.Zero);
        Tick();

        if (SelfTest)
            Native.SetTimer(_hwnd, new IntPtr(2), (uint)(_selfTestSeconds * 1000), IntPtr.Zero);
        else if (!AddTray())
            Log.Warn("托盘图标添加失败");

        while (Native.GetMessageW(out MSG msg, IntPtr.Zero, 0, 0))
        {
            Native.TranslateMessage(ref msg);
            Native.DispatchMessageW(ref msg);
        }

        Cleanup();
        return 0;
    }

    private static string AcText(byte v) => v switch { 1 => "交流", 0 => "电池", _ => "未知" };

    private static void RegisterNotify(ref Guid g)
    {
        IntPtr h = Native.RegisterPowerSettingNotification(_hwnd, ref g, 0);
        if (h != IntPtr.Zero) Notifies.Add(h);
        else Log.Err($"注册电源通知失败 {g}");
    }

    private static IntPtr CurrentIcon()
    {
        if (_holding) return _hIconActive;
        return BlockedReason() != null ? _hIconPaused : _hIconIdle;
    }

    private static bool AddTray()
    {
        var data = new NOTIFYICONDATAW
        {
            cbSize = (uint)Marshal.SizeOf<NOTIFYICONDATAW>(),
            hWnd = _hwnd,
            uID = 1,
            uFlags = Native.NIF_MESSAGE | Native.NIF_ICON | Native.NIF_TIP,
            uCallbackMessage = 0x0400 + 1,
            hIcon = CurrentIcon(),
            szTip = StatusLine(),
        };
        bool ok = Native.Shell_NotifyIconW(Native.NIM_ADD, ref data);
        _trayAdded = ok;
        _lastTip = data.szTip;
        Log.Info(ok ? "托盘图标已添加" : "Shell_NotifyIconW 失败");
        return ok;
    }

    private static void UpdateTray()
    {
        if (!_trayAdded) return;
        string tip = StatusLine();
        var data = new NOTIFYICONDATAW
        {
            cbSize = (uint)Marshal.SizeOf<NOTIFYICONDATAW>(),
            hWnd = _hwnd,
            uID = 1,
            uFlags = Native.NIF_ICON | Native.NIF_TIP,
            hIcon = CurrentIcon(),
            szTip = tip,
        };
        Native.Shell_NotifyIconW(Native.NIM_MODIFY, ref data);
        _lastTip = tip;
    }

    // -- 逻辑 ---------------------------------------------------------------

    private static string BlockedReason()
    {
        double now = Now;
        if (_lidClosed) return "合盖";
        if (_locked) return "已锁屏";
        if (now < _pausedUntil) return $"已暂停（剩 {(int)(_pausedUntil - now)} 秒）";
        if (now < _allowUntil) return "已放行本次睡眠";
        return null;
    }

    private static string StatusLine()
    {
        string r = BlockedReason();
        if (r != null) return "未保护 · " + r;
        if (_holding) return _battery >= 0 ? $"保护中 · 电源接入 · 电量 {_battery}%" : "保护中 · 电源接入";
        return _acOnline ? "准备接管…" : "未接电 · 已交还系统策略";
    }

    private static string StatsLine() => $"误报掉电 {_glitch} 次 · 息屏 {_monitorOff} 次 · 待机 {_suspend} 次";

    private static void Apply()
    {
        double now = Now;
        _stickyAc = (now - _lastAcSeen) < AcStickySeconds;
        bool want = _stickyAc && BlockedReason() == null;
        if (want && !_holding)
        {
            if (_req.Acquire())
            {
                _holding = true;
                Log.Info("已接管：持有显示 + 系统电源请求");
                UpdateTray();
            }
        }
        else if (!want && _holding)
        {
            _req.Release();
            _holding = false;
            Log.Info("已释放电源请求（" + (BlockedReason() ?? "不再满足接电条件") + "）");
            UpdateTray();
        }
    }

    private static void Tick()
    {
        if (Native.GetSystemPowerStatus(out SYSTEM_POWER_STATUS st))
        {
            bool online = st.ACLineStatus == 1;
            _acOnline = online;
            _battery = st.BatteryLifePercent <= 100 ? st.BatteryLifePercent : -1;
            if (online)
            {
                _lastAcSeen = Now;
                if (_havePrev && !_prevOnline)
                {
                    _glitch++;
                    Log.Warn($"AC 恢复（此前被误报断开）—— 第 {_glitch} 次跳变");
                }
            }
            else if (_havePrev && _prevOnline)
            {
                Log.Warn("AC 断开（sticky 窗口内继续保护）");
            }
            _prevOnline = online;
            _havePrev = true;
        }
        Apply();
    }

    // -- 菜单 ---------------------------------------------------------------

    private static void ShowMenu()
    {
        IntPtr menu = Native.CreatePopupMenu();
        Native.AppendMenuW(menu, Native.MF_STRING | Native.MF_GRAYED, IntPtr.Zero, StatusLine());
        Native.AppendMenuW(menu, Native.MF_STRING | Native.MF_GRAYED, IntPtr.Zero, StatsLine());
        Native.AppendMenuW(menu, Native.MF_SEPARATOR, IntPtr.Zero, null);
        bool blocked = BlockedReason() != null;
        Native.AppendMenuW(menu, Native.MF_STRING, new IntPtr(IdTogglePause),
            blocked ? "恢复保护" : "暂停保护 30 分钟");
        Native.AppendMenuW(menu, Native.MF_STRING | (Now < _allowUntil ? Native.MF_CHECKED : 0),
            new IntPtr(IdAllowOnce), "放行下一次睡眠");
        Native.AppendMenuW(menu, Native.MF_STRING | (ScreenSaverReallyEnabled() ? Native.MF_CHECKED : 0),
            new IntPtr(IdScreenSaver), "允许屏幕保护（不建议）");
        Native.AppendMenuW(menu, Native.MF_STRING | (AutoStartEnabled() ? Native.MF_CHECKED : 0),
            new IntPtr(IdAutoStart), "开机自启动");
        Native.AppendMenuW(menu, Native.MF_SEPARATOR, IntPtr.Zero, null);
        Native.AppendMenuW(menu, Native.MF_STRING, new IntPtr(IdOpenLog), "打开日志目录");
        Native.AppendMenuW(menu, Native.MF_STRING, new IntPtr(IdExit), "退出");

        Native.GetCursorPos(out POINT p);
        Native.SetForegroundWindow(_hwnd);
        int cmd = Native.TrackPopupMenu(menu,
            Native.TPM_LEFTALIGN | Native.TPM_RIGHTBUTTON | Native.TPM_RETURNCMD,
            p.X, p.Y, 0, _hwnd, IntPtr.Zero);
        Native.PostMessageW(_hwnd, Native.WM_NULL, IntPtr.Zero, IntPtr.Zero);
        Native.DestroyMenu(menu);

        switch ((uint)cmd)
        {
            case IdTogglePause:
                if (blocked) { _pausedUntil = 0; _allowUntil = 0; Log.Info("恢复保护"); }
                else { _pausedUntil = Now + 30 * 60; Log.Info("暂停保护 30 分钟"); }
                Apply();
                break;
            case IdAllowOnce:
                _allowUntil = Now + ManualAllowTimeout;
                Log.Info("放行下一次睡眠");
                Apply();
                break;
            case IdScreenSaver:
                SetScreenSaver(!ScreenSaverReallyEnabled());
                break;
            case IdAutoStart:
                SetAutoStart(!AutoStartEnabled());
                break;
            case IdOpenLog:
                try
                {
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
                    { FileName = "explorer.exe", Arguments = Path.GetDirectoryName(Log.Path_) });
                }
                catch { }
                break;
            case IdExit:
                Log.Info($"退出 | 运行 {(DateTime.Now - Start).TotalMinutes:F1} 分钟");
                Native.PostQuitMessage(0);
                break;
        }
        UpdateTray();
    }

    // -- 设置 ---------------------------------------------------------------

    private static string RegGet(string sub, string name)
    {
        try
        {
            using var k = Registry.CurrentUser.OpenSubKey(sub);
            return k?.GetValue(name) as string;
        }
        catch { return null; }
    }

    private static bool RegSet(string sub, string name, string value)
    {
        try
        {
            using var k = Registry.CurrentUser.CreateSubKey(sub, true);
            k.SetValue(name, value, RegistryValueKind.String);
            return true;
        }
        catch (Exception e) { Log.Err("写注册表失败: " + e.Message); return false; }
    }

    /// <summary>屏保要真的生效，三个条件缺一不可。</summary>
    public static bool ScreenSaverReallyEnabled()
    {
        string active = RegGet(@"Control Panel\Desktop", "ScreenSaveActive");
        if (active != "1") return false;
        string exe = RegGet(@"Control Panel\Desktop", "SCRNSAVE.EXE");
        if (string.IsNullOrWhiteSpace(exe)) return false;
        string timeout = RegGet(@"Control Panel\Desktop", "ScreenSaveTimeOut");
        return int.TryParse(timeout, out int t) && t > 0;
    }

    private static void SetScreenSaver(bool enable)
    {
        RegSet(@"Control Panel\Desktop", "ScreenSaveActive", enable ? "1" : "0");
        Log.Info($"屏幕保护 -> {(enable ? "允许" : "禁止")}");
    }

    private static bool AutoStartEnabled()
    {
        string v = RegGet(@"Software\Microsoft\Windows\CurrentVersion\Run", "WakeGuard");
        return !string.IsNullOrEmpty(v);
    }

    private static void SetAutoStart(bool enable)
    {
        string sub = @"Software\Microsoft\Windows\CurrentVersion\Run";
        try
        {
            using var k = Registry.CurrentUser.CreateSubKey(sub, true);
            if (enable)
            {
                string exe = Environment.ProcessPath ?? "WakeGuard.exe";
                k.SetValue("WakeGuard", $"\"{exe}\"", RegistryValueKind.String);
            }
            else k.DeleteValue("WakeGuard", false);
            Log.Info($"开机自启动 -> {enable}");
        }
        catch (Exception e) { Log.Err("设置自启动失败: " + e.Message); }
    }

    // -- 窗口过程 -----------------------------------------------------------

    private static IntPtr WindowProc(IntPtr h, uint m, IntPtr w, IntPtr l)
    {
        switch (m)
        {
            case 0x0401: // WM_USER+1 托盘
                if ((uint)l.ToInt64() == Native.WM_RBUTTONUP) ShowMenu();
                else if ((uint)l.ToInt64() == Native.WM_LBUTTONUP) UpdateTray();
                return IntPtr.Zero;

            case Native.WM_TIMER:
                if (w == TimerId) Tick();
                else if (SelfTest)
                {
                    Log.Info($"SELFTEST RESULT holding={_holding} ac={_acOnline} " +
                             $"sticky={_stickyAc} glitch={_glitch} | {StatusLine()} | {StatsLine()}");
                    Native.PostQuitMessage(0);
                }
                return IntPtr.Zero;

            case Native.WM_POWERBROADCAST:
                OnPower(w, l);
                return new IntPtr(1);

            case Native.WM_WTSSESSION_CHANGE:
                uint code = (uint)w.ToInt64();
                if (code == Native.WTS_SESSION_LOCK) { _locked = true; Log.Info("会话已锁定，交还电源策略"); }
                else if (code == Native.WTS_SESSION_UNLOCK) { _locked = false; Log.Info("会话已解锁"); }
                else return IntPtr.Zero;
                Apply();
                return IntPtr.Zero;

            case Native.WM_DESTROY:
                Native.PostQuitMessage(0);
                return IntPtr.Zero;
        }
        return Native.DefWindowProcW(h, m, w, l);
    }

    private static void OnPower(IntPtr w, IntPtr l)
    {
        uint evt = (uint)w.ToInt64();
        if (evt == Native.PBT_POWERSETTINGCHANGE && l != IntPtr.Zero)
        {
            var s = Marshal.PtrToStructure<POWERBROADCAST_SETTING>(l);
            int off = Marshal.OffsetOf<POWERBROADCAST_SETTING>("Data").ToInt32();
            byte value = Marshal.ReadByte(l, off);
            if (s.PowerSetting == GuidAcDc)
            {
                if (value == 1) { _lastAcSeen = Now; _acOnline = true; }
                else _acOnline = false;
            }
            else if (s.PowerSetting == GuidMonitor)
            {
                if (value == 0)
                {
                    _monitorOff++;
                    Log.Warn($"显示器已关闭（第 {_monitorOff} 次）holding={_holding}");
                }
            }
            else if (s.PowerSetting == GuidLid)
            {
                _lidClosed = value == 0;
                Log.Info("盖子：" + (_lidClosed ? "关闭" : "打开"));
            }
            Apply();
        }
        else if (evt == Native.PBT_APMSUSPEND)
        {
            _suspend++;
            Log.Warn($"系统进入待机（第 {_suspend} 次）");
        }
        else if (evt == Native.PBT_APMRESUMEAUTOMATIC || evt == Native.PBT_APMRESUMESUSPEND)
        {
            Log.Info("系统已恢复");
            _allowUntil = 0;
            Apply();
        }
    }

    private static void Cleanup()
    {
        _req?.Release();
        _req?.Dispose();
        if (_trayAdded)
        {
            var data = new NOTIFYICONDATAW
            {
                cbSize = (uint)Marshal.SizeOf<NOTIFYICONDATAW>(),
                hWnd = _hwnd, uID = 1,
            };
            Native.Shell_NotifyIconW(Native.NIM_DELETE, ref data);
        }
        foreach (var n in Notifies) Native.UnregisterPowerSettingNotification(n);
        Notifies.Clear();
        if (_hwnd != IntPtr.Zero) Native.WTSUnRegisterSessionNotification(_hwnd);
        Native.DestroyIcon(_hIconActive);
        Native.DestroyIcon(_hIconIdle);
        Native.DestroyIcon(_hIconPaused);
        Log.Info("已退出");
    }
}
