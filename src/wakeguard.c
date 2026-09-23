/*
 * WakeGuard (C / Win32) — 缓解 KB5121003 (26200.9168) 误报 AC 断开导致的息屏。
 *
 * 用户态无法否决已进入的睡眠转换，因此采取预防式：持续持有 PowerRequest
 * (DisplayRequired + SystemRequired)，并对 AC 状态做 60 秒去抖，
 * 吸收 1~5 秒的误报。锁屏 / 合盖 / 菜单放行时主动释放。
 *
 * 编译：gcc -O2 -s -mwindows -fexec-charset=UTF-8 -finput-charset=UTF-8 \
 *        -o WakeGuardC.exe wakeguard.c -luser32 -lshell32 -ladvapi32 -lgdi32
 */

#define _WIN32_WINNT 0x0600
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>   /* NOTIFYICONDATAW / Shell_NotifyIconW / ShellExecuteW */
#include <shlobj.h>     /* SHGetFolderPathW / IShellLinkW */
#include <objidl.h>
#include <oleauto.h>    /* SysAllocString / SysFreeString */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ---------------------------------------------------------------- 常量 */

#define WG_VERSION          "1.2"

#define AC_STICKY_MS        60000
#define POLL_MS             500
#define MANUAL_ALLOW_MS     300000
#define PAUSE_MS            (30 * 60 * 1000)

#define RUN_SUBKEY          L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

#define TRAY_MSG            (WM_USER + 1)
#define TIMER_POLL          1
#define TIMER_SELFTEST      2
#define TIMER_TRAY_RETRY    3

#define TRAY_RETRY_MAX      3       /* NIM_ADD 失败后的重试次数 */
#define TRAY_RETRY_MS       2000

#define ID_TOGGLE_PAUSE     1001
#define ID_ALLOW_ONCE       1002
#define ID_SCREENSAVER      1003
#define ID_AUTOSTART        1004
#define ID_OPENLOG          1005
#define ID_EXIT             1006
#define ID_AUTOSTART_FOLDER 1007
#define ID_DIAGNOSE         1008
#define ID_AUTOSTART_TASK   1009
#define ID_AUTOSTART_OFF    1010
#define ID_AUTOSTART_AUTO   1011

#define REQ_DISPLAY         0
#define REQ_SYSTEM          1
#define POWER_REQUEST_CONTEXT_SIMPLE_STRING 0x00000001

#ifndef ERROR_ALREADY_EXISTS
#define ERROR_ALREADY_EXISTS 183
#endif
#ifndef WTS_SESSION_LOCK
#define WTS_SESSION_LOCK    0x7
#define WTS_SESSION_UNLOCK  0x8
#endif

/* ------------------------------------------------------------ 类型定义 */

/* POWER_REQUEST_TYPE 与 REASON_CONTEXT 已由 winnt.h / minwinbase.h 提供 */

/* 自启条目三态：未启用 / 已生效 / 僵尸（条目还在，但指向的 exe 已不存在） */
typedef enum { AS_OFF = 0, AS_ON = 1, AS_STALE = 2 } AutoState;

/* 自启后端优先级：数字越小越优先 */
typedef enum { AST_TASK = 0, AST_FOLDER = 1, AST_REG = 2, AST_NONE = 3 } AutoBackend;

typedef struct {
    GUID  PowerSetting;
    DWORD DataLength;
    BYTE  Data[1];
} PBT_SETTING;

/* 动态加载的函数指针（避免依赖较新的导入库） */
typedef HANDLE (WINAPI *PFN_PCR)(REASON_CONTEXT *);
typedef BOOL   (WINAPI *PFN_PSR)(HANDLE, POWER_REQUEST_TYPE);
typedef HANDLE (WINAPI *PFN_RPSN)(HANDLE, LPCGUID, DWORD);
typedef BOOL   (WINAPI *PFN_UPSN)(HANDLE);
typedef BOOL   (WINAPI *PFN_WTSREG)(HWND, DWORD);
typedef BOOL   (WINAPI *PFN_WTSUNREG)(HWND);
typedef HICON  (WINAPI *PFN_CIFRE)(PBYTE, DWORD, BOOL, DWORD, int, int, UINT);

static PFN_PCR       pPowerCreateRequest = NULL;
static PFN_PSR       pPowerSetRequest = NULL;
static PFN_PSR       pPowerClearRequest = NULL;
static PFN_RPSN      pRegPowerSetting = NULL;
static PFN_UPSN      pUnregPowerSetting = NULL;
static PFN_WTSREG    pWTSRegister = NULL;
static PFN_WTSUNREG  pWTSUnRegister = NULL;
static PFN_CIFRE     pCreateIconFromResourceEx = NULL;

static const GUID GUID_ACDC = {
    0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48} };
static const GUID GUID_MONITOR = {
    0x02731015, 0x4510, 0x4526, {0x99, 0xE6, 0xE5, 0xA1, 0x7E, 0xBD, 0x1E, 0xA6} };
static const GUID GUID_LID = {
    0xBA3E0F4D, 0xB817, 0x4094, {0xA2, 0xA1, 0xD5, 0x63, 0x86, 0xE4, 0xAE, 0x7D} };

/* ------------------------------------------------------------ 全局状态 */

static HWND   g_hwnd = NULL;
static HANDLE g_req = NULL;
static int    g_holding = 0;
static ULONGLONG g_lastAcSeen = 0;
static int    g_prevOnline = -1;
static ULONGLONG g_pausedUntil = 0;
static ULONGLONG g_allowUntil = 0;
static int    g_lidClosed = 0;
static int    g_locked = 0;
static int    g_acOnline = 0;
static int    g_battery = -1;
static int    g_glitch = 0, g_monitorOff = 0, g_suspend = 0;
static int    g_selfTest = 0;
static LONG   g_lastRegError = -1;   /* -1 = 本次运行未尝试写入 */
static HICON  g_iconActive = NULL, g_iconIdle = NULL, g_iconPaused = NULL;
static int    g_trayAdded = 0;
static int    g_trayRetry = 0;
static UINT   g_msgTaskbarCreated = 0;   /* RegisterWindowMessage(L"TaskbarCreated") */
static HANDLE g_mutex = NULL;
static HANDLE g_notify[3] = {NULL, NULL, NULL};
static FILE  *g_log = NULL;
static char   g_logDir[MAX_PATH * 2] = {0};
static char   g_logPath[MAX_PATH * 2 + 32] = {0};

/* ------------------------------------------------------------ 工具函数 */

static ULONGLONG Now(void) { return GetTickCount64(); }

static void LogOpen(void)
{
    WCHAR appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH)) return;
    char dir[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, appdata, -1, dir, MAX_PATH, NULL, NULL);
    snprintf(g_logDir, sizeof(g_logDir), "%s\\WakeGuard", dir);
    snprintf(g_logPath, sizeof(g_logPath), "%s\\wakeguard.log", g_logDir);
    CreateDirectoryA(g_logDir, NULL);
    g_log = fopen(g_logPath, "a");
}

static void Log(const char *level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (g_log) {
        fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, level, buf);
        fflush(g_log);
    }
}

/* UTF-8 字面量 -> UTF-16（轮换缓冲区，调用后不需释放） */
static WCHAR *W_(const char *s)
{
    static WCHAR bufs[16][256];
    static int idx = 0;
    WCHAR *b = bufs[idx];
    idx = (idx + 1) % 16;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, b, 256);
    return b;
}

/* ---------------------------------------------------------- 图标构造 */

static const int BOLT[6][2] = { {19,4}, {10,18}, {15,18}, {13,29}, {22,12}, {16,12} };

static int InBolt(int x, int y)
{
    int inside = 0, i, j = 5;
    for (i = 0; i < 6; j = i++) {
        int xi = BOLT[i][0], yi = BOLT[i][1], xj = BOLT[j][0], yj = BOLT[j][1];
        if (((yi > y) != (yj > y)) && (x < (double)(xj - xi) * (y - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

static void Put32(BYTE *p, UINT v)
{
    p[0] = (BYTE)(v & 0xFF); p[1] = (BYTE)((v >> 8) & 0xFF);
    p[2] = (BYTE)((v >> 16) & 0xFF); p[3] = (BYTE)((v >> 24) & 0xFF);
}

/* 构造 RT_ICON 资源体：32x32 32bpp，实心圆 + 白色闪电 */
static HICON MakeIcon(int r, int g, int b)
{
    const int W = 32, H = 32;
    int xorBytes = W * H * 4, andBytes = 128;
    int total = 40 + xorBytes + andBytes;
    BYTE *res = (BYTE *)calloc(total, 1);
    HICON h = NULL;
    int x, y;
    if (!res) return NULL;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            int i = (y * W + x) * 4;
            double dx = x - 15.5, dy = y - 15.5;
            int bolt;
            if (dx * dx + dy * dy > 15.5 * 15.5) continue;
            bolt = InBolt(x, y);
            res[40 + i + 0] = (BYTE)(bolt ? 255 : b);
            res[40 + i + 1] = (BYTE)(bolt ? 255 : g);
            res[40 + i + 2] = (BYTE)(bolt ? 255 : r);
            res[40 + i + 3] = 255;
        }
    }
    Put32(res + 0, 40);
    Put32(res + 4, (UINT)W);
    Put32(res + 8, (UINT)(H * 2));
    res[12] = 1; res[13] = 0;
    res[14] = 32; res[15] = 0;
    Put32(res + 20, (UINT)(xorBytes + andBytes));

    if (pCreateIconFromResourceEx)
        h = pCreateIconFromResourceEx(res, (DWORD)total, TRUE, 0x00030000, 0, 0, 0x00008000);
    if (!h) h = LoadIconW(NULL, (LPCWSTR)(INT_PTR)32512); /* IDI_APPLICATION */
    free(res);
    return h;
}

/* ---------------------------------------------------------- 电源请求 */

static void Acquire(void)
{
    if (!g_req) {
        REASON_CONTEXT ctx;
        static WCHAR reason[] = L"WakeGuard: preventing KB5121003 false AC-disconnect blanking";
        ZeroMemory(&ctx, sizeof(ctx));
        ctx.Version = 0;
        ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        ctx.Reason.SimpleReasonString = reason;
        g_req = pPowerCreateRequest(&ctx);
        if (!g_req) { Log("ERROR", "PowerCreateRequest failed"); return; }
    }
    pPowerSetRequest(g_req, PowerRequestDisplayRequired);
    pPowerSetRequest(g_req, PowerRequestSystemRequired);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    g_holding = 1;
    Log("INFO", "acquired display+system power request");
}

static void Release(const char *why)
{
    if (g_req) {
        pPowerClearRequest(g_req, PowerRequestDisplayRequired);
        pPowerClearRequest(g_req, PowerRequestSystemRequired);
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    g_holding = 0;
    Log("INFO", "released power request (%s)", why ? why : "condition no longer met");
}

/* ------------------------------------------------------------ 状态文本 */

static const char *BlockedReason(void)
{
    static char pauseBuf[32];
    ULONGLONG now = Now();
    if (g_lidClosed) return "合盖";
    if (g_locked) return "已锁屏";
    if (now < g_pausedUntil) {
        snprintf(pauseBuf, sizeof(pauseBuf), "已暂停（剩 %d 秒）",
                 (int)((g_pausedUntil - now) / 1000));
        return pauseBuf;
    }
    if (now < g_allowUntil) return "已放行本次睡眠";
    return NULL;
}

static void StatusLine(char *out, int n)
{
    const char *r = BlockedReason();
    if (r) { snprintf(out, n, "未保护 · %s", r); return; }
    if (g_holding) {
        if (g_battery >= 0) snprintf(out, n, "保护中 · 电源接入 · 电量 %d%%", g_battery);
        else snprintf(out, n, "保护中 · 电源接入");
        return;
    }
    if (g_acOnline) snprintf(out, n, "准备接管…");
    else snprintf(out, n, "未接电 · 已交还系统策略");
}

static void TrayUpdate(void);   /* 供 Tick() 提前调用 */

/* 托盘提示：第一行状态，第二行统计（explorer 的 tooltip 支持 \r\n 换行） */
static void StatusTip(WCHAR *out, int nChars)
{
    char l1[128], l2[128], buf[300];
    StatusLine(l1, sizeof(l1));
    snprintf(l2, sizeof(l2), "误报掉电 %d 次 · 息屏 %d 次 · 待机 %d 次",
             g_glitch, g_monitorOff, g_suspend);
    snprintf(buf, sizeof(buf), "%s\r\n%s", l1, l2);
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, nChars);
}

static void Apply(void)
{
    ULONGLONG now = Now();
    int sticky = (now - g_lastAcSeen) < AC_STICKY_MS;
    const char *blocked = BlockedReason();
    int want = sticky && !blocked;
    if (want && !g_holding) Acquire();
    else if (!want && g_holding) Release(blocked);
    (void)now;
}

static void Tick(void)
{
    SYSTEM_POWER_STATUS st;
    if (GetSystemPowerStatus(&st)) {
        int online = (st.ACLineStatus == 1);
        g_acOnline = online;
        g_battery = (st.BatteryLifePercent <= 100) ? st.BatteryLifePercent : -1;
        if (online) {
            g_lastAcSeen = Now();
            if (g_prevOnline == 0) {
                g_glitch++;
                Log("WARN", "AC back after false disconnect - glitch #%d", g_glitch);
            }
        } else if (g_prevOnline == 1) {
            Log("WARN", "AC reported disconnected (still protecting, sticky window)");
        }
        g_prevOnline = online;
    }
    Apply();
    TrayUpdate();   /* 统计数字变化后刷新悬停提示 */
}

/* ---------------------------------------------------------- 托盘 / 菜单 */

static HICON CurrentIcon(void)
{
    if (g_holding) return g_iconActive;
    return BlockedReason() ? g_iconPaused : g_iconIdle;
}

static WCHAR g_lastTip[128] = {0};
static HICON g_lastIcon = NULL;

static void TrayRemove(void);

/* 单次添加尝试。开机自启时 Explorer 托盘可能尚未就绪，故允许反复调用。 */
static int TrayAddOnce(void)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = TRAY_MSG;
    nid.hIcon = CurrentIcon();
    StatusTip(nid.szTip, 128);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return 0;
    g_trayAdded = 1;
    lstrcpyW(g_lastTip, nid.szTip);
    g_lastIcon = nid.hIcon;
    return 1;
}

static void TrayAdd(void)
{
    g_trayAdded = 0;
    g_trayRetry = 0;
    if (TrayAddOnce()) { Log("INFO", "tray icon added"); return; }
    Log("WARN", "Shell_NotifyIconW(NIM_ADD) failed, scheduling retry");
    SetTimer(g_hwnd, TIMER_TRAY_RETRY, TRAY_RETRY_MS, NULL);
}

/* Explorer 崩溃重启 / 资源管理器重建后触发：先删后加，避免留下幽灵图标 */
static void TrayReAdd(void)
{
    if (g_trayAdded) TrayRemove();
    g_trayAdded = 0;
    g_trayRetry = 0;
    if (TrayAddOnce()) { Log("INFO", "tray icon re-added (TaskbarCreated)"); return; }
    Log("WARN", "tray re-add failed, scheduling retry");
    SetTimer(g_hwnd, TIMER_TRAY_RETRY, TRAY_RETRY_MS, NULL);
}

/* 仅在提示文本或图标真正变化时才通知 Shell，避免每 500ms 无谓刷新 */
static void TrayUpdate(void)
{
    NOTIFYICONDATAW nid;
    if (!g_trayAdded) return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = CurrentIcon();
    StatusTip(nid.szTip, 128);
    if (wcscmp(nid.szTip, g_lastTip) == 0 && nid.hIcon == g_lastIcon) return;
    lstrcpyW(g_lastTip, nid.szTip);
    g_lastIcon = nid.hIcon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid;
    if (!g_trayAdded) return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

/* -------------------------------------------------------------- 注册表 */

static int RegGetString(HKEY root, const WCHAR *sub, const WCHAR *name, char *out, int n)
{
    HKEY k; LONG rc; WCHAR buf[512]; DWORD type = 0, size = sizeof(buf);
    out[0] = 0;
    rc = RegOpenKeyExW(root, sub, 0, KEY_READ, &k);
    if (rc != ERROR_SUCCESS) return 0;
    rc = RegQueryValueExW(k, name, NULL, &type, (LPBYTE)buf, &size);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return 0;
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, n, NULL, NULL);
    return 1;
}

/* 返回 Win32 错误码，0 表示成功 */
static LONG RegSetString(HKEY root, const WCHAR *sub, const WCHAR *name, const char *val)
{
    HKEY k; LONG rc; DWORD disp; WCHAR w[512]; DWORD size;
    MultiByteToWideChar(CP_UTF8, 0, val, -1, w, 512);
    rc = RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, &disp);
    if (rc != ERROR_SUCCESS) return rc;
    size = (DWORD)((wcslen(w) + 1) * sizeof(WCHAR));
    rc = RegSetValueExW(k, name, 0, REG_SZ, (LPBYTE)w, size);
    RegCloseKey(k);
    return rc;
}

static int RegDelete(HKEY root, const WCHAR *sub, const WCHAR *name)
{
    HKEY k; LONG rc;
    rc = RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &k);
    if (rc != ERROR_SUCCESS) return 0;
    rc = RegDeleteValueW(k, name);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

/* 返回 Win32 错误码，0 成功 */
static LONG RegDeleteEx(HKEY root, const WCHAR *sub, const WCHAR *name)
{
    HKEY k; LONG rc;
    rc = RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &k);
    if (rc != ERROR_SUCCESS) return rc;
    rc = RegDeleteValueW(k, name);
    RegCloseKey(k);
    return rc;
}

/* 屏保要真生效：开关 + 程序 + 超时，三者缺一不可 */
static int ScreenSaverReallyOn(void)
{
    char a[64] = {0}, e[512] = {0}, t[64] = {0};
    RegGetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaveActive", a, sizeof(a));
    if (strcmp(a, "1") != 0) return 0;
    RegGetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"SCRNSAVE.EXE", e, sizeof(e));
    if (e[0] == 0) return 0;
    RegGetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaveTimeOut", t, sizeof(t));
    return atoi(t) > 0;
}

static int FileExistsW(const WCHAR *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* 从命令行串中取出可执行文件路径，支持带引号与不带引号两种写法 */
static void ParseCommandPath(const char *cmd, char *out, int n)
{
    int i = 0, k = 0;
    out[0] = 0;
    while (cmd[i] == ' ') i++;
    if (cmd[i] == '"') {
        i++;
        while (cmd[i] && cmd[i] != '"' && k < n - 1) out[k++] = cmd[i++];
    } else {
        while (cmd[i] && cmd[i] != ' ' && k < n - 1) out[k++] = cmd[i++];
    }
    out[k] = 0;
}

/* 当前 exe 的绝对路径（UTF-8） */
static void SelfPathUtf8(char *out, int n)
{
    WCHAR w[MAX_PATH * 2];
    GetModuleFileNameW(NULL, w, MAX_PATH * 2);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
}

/* 当前 exe 所在目录（UTF-8，不含尾部分隔符） */
static void SelfDirUtf8(char *out, int n)
{
    char path[MAX_PATH * 2];
    int i, last = -1;
    SelfPathUtf8(path, sizeof(path));
    for (i = 0; path[i]; i++) if (path[i] == '\\') last = i;
    if (last < 0) { snprintf(out, n, "."); return; }
    snprintf(out, n, "%.*s", last, path);
}

/* 三态：条目不存在=OFF；存在且目标文件还在=ON；存在但目标已消失=STALE */
static AutoState AutoStartOn(void)
{
    char v[512] = {0}, path[MAX_PATH * 2] = {0};
    WCHAR w[MAX_PATH * 2];
    if (!RegGetString(HKEY_CURRENT_USER, RUN_SUBKEY, L"WakeGuard", v, sizeof(v)))
        return AS_OFF;
    ParseCommandPath(v, path, sizeof(path));
    if (path[0] == 0) return AS_OFF;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH * 2);
    return FileExistsW(w) ? AS_ON : AS_STALE;
}

/* 返回 0 成功，非 0 为 Win32 错误码 */
static LONG SetAutoStart(int on)
{
    LONG rc;
    if (on) {
        char path[MAX_PATH * 2], quoted[MAX_PATH * 2 + 4];
        SelfPathUtf8(path, sizeof(path));
        snprintf(quoted, sizeof(quoted), "\"%s\"", path);
        rc = RegSetString(HKEY_CURRENT_USER, RUN_SUBKEY, L"WakeGuard", quoted);
        g_lastRegError = rc;
        Log("INFO", "reg autostart set rc=%ld value=%s", rc, quoted);
    } else {
        rc = RegDeleteEx(HKEY_CURRENT_USER, RUN_SUBKEY, L"WakeGuard");
        g_lastRegError = rc;
        Log("INFO", "reg autostart delete rc=%ld", rc);
    }
    Log("INFO", "autostart(reg) -> %s (now %d)", on ? "on" : "off", AutoStartOn());
    return rc;
}

/* ---------- 备选路径：启动文件夹快捷方式（绕开注册表 Run 键） ---------- */

static void StartupFolder(char *out, int n)
{
    WCHAR w[MAX_PATH];
    out[0] = 0;
    if (SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, SHGFP_TYPE_CURRENT, w) == S_OK)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
}

/* 读取 .lnk 指向的目标路径；成功返回 1 */
static int ReadShortcutTarget(const WCHAR *linkPath, WCHAR *out, int nChars)
{
    IShellLinkW *psl = NULL;
    IPersistFile *ppf = NULL;
    HRESULT hr;
    int ok = 0;
    out[0] = 0;

    CoInitialize(NULL);
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkW, (void **)&psl);
    if (SUCCEEDED(hr)) {
        hr = psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, (void **)&ppf);
        if (SUCCEEDED(hr)) {
            if (SUCCEEDED(ppf->lpVtbl->Load(ppf, linkPath, STGM_READ))) {
                if (FAILED(psl->lpVtbl->GetPath(psl, out, nChars, NULL, 0)) || !out[0]) {
                    /* 目标可能尚未解析，静默重试一次 */
                    out[0] = 0;
                    if (SUCCEEDED(psl->lpVtbl->Resolve(psl, NULL, 0x1 /* SLR_NO_UI */)))
                        psl->lpVtbl->GetPath(psl, out, nChars, NULL, 0);
                }
                ok = (out[0] != 0);
            }
            ppf->lpVtbl->Release(ppf);
        }
        psl->lpVtbl->Release(psl);
    }
    CoUninitialize();
    return ok;
}

static int CreateShortcut(const WCHAR *target, const WCHAR *dir,
                          const WCHAR *linkPath, const WCHAR *desc)
{
    IShellLinkW *psl = NULL;
    IPersistFile *ppf = NULL;
    HRESULT hr;
    int ok = 0;

    CoInitialize(NULL);
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkW, (void **)&psl);
    if (SUCCEEDED(hr)) {
        psl->lpVtbl->SetPath(psl, target);
        psl->lpVtbl->SetWorkingDirectory(psl, dir);
        psl->lpVtbl->SetIconLocation(psl, target, 0);
        psl->lpVtbl->SetDescription(psl, desc);
        hr = psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, (void **)&ppf);
        if (SUCCEEDED(hr)) {
            ok = SUCCEEDED(ppf->lpVtbl->Save(ppf, linkPath, TRUE));
            ppf->lpVtbl->Release(ppf);
        }
        psl->lpVtbl->Release(psl);
    } else {
        Log("ERROR", "CoCreateInstance ShellLink hr=0x%08lX", (unsigned long)hr);
    }
    CoUninitialize();
    return ok;
}

static int SetAutoStartFolder(int on)
{
    char folder[MAX_PATH];
    WCHAR wLink[MAX_PATH], wTarget[MAX_PATH], wDir[MAX_PATH];
    WCHAR wFolder[MAX_PATH];
    char linkPath[MAX_PATH * 2];
    DWORD attr;

    StartupFolder(folder, sizeof(folder));
    if (folder[0] == 0) { Log("ERROR", "cannot resolve startup folder"); return 0; }
    snprintf(linkPath, sizeof(linkPath), "%s\\WakeGuard.lnk", folder);

    MultiByteToWideChar(CP_UTF8, 0, linkPath, -1, wLink, MAX_PATH);
    if (!on) {
        int ok = DeleteFileW(wLink) ? 1 : 0;
        Log("INFO", "startup folder link delete=%d", ok);
        return ok;
    }
    GetModuleFileNameW(NULL, wTarget, MAX_PATH);
    /* 工作目录 = exe 所在目录 */
    {
        int i, last = 0;
        for (i = 0; wTarget[i]; i++) if (wTarget[i] == L'\\') last = i;
        wcsncpy(wDir, wTarget, last);
        wDir[last] = 0;
    }
    if (!CreateShortcut(wTarget, wDir, wLink, L"WakeGuard")) {
        Log("ERROR", "CreateShortcut failed");
        return 0;
    }
    MultiByteToWideChar(CP_UTF8, 0, folder, -1, wFolder, MAX_PATH);
    attr = GetFileAttributesW(wLink);
    Log("INFO", "startup folder link created attr=0x%lX", (unsigned long)attr);
    return attr != INVALID_FILE_ATTRIBUTES;
}

/* 三态：快捷方式不存在=OFF；存在且指向的 exe 还在=ON；指向已失效=STALE */
static AutoState AutoStartFolderOn(void)
{
    char folder[MAX_PATH], linkPath[MAX_PATH * 2];
    WCHAR wLink[MAX_PATH], wTarget[MAX_PATH];
    StartupFolder(folder, sizeof(folder));
    if (folder[0] == 0) return AS_OFF;
    snprintf(linkPath, sizeof(linkPath), "%s\\WakeGuard.lnk", folder);
    MultiByteToWideChar(CP_UTF8, 0, linkPath, -1, wLink, MAX_PATH);
    if (!FileExistsW(wLink)) return AS_OFF;
    if (!ReadShortcutTarget(wLink, wTarget, MAX_PATH)) return AS_STALE;
    return FileExistsW(wTarget) ? AS_ON : AS_STALE;
}

/* ------------------- 第三条路径：任务计划程序（Task Scheduler 2.0） ------
 *
 * 只用到 ITaskService::Connect / GetFolder 与 ITaskFolder::RegisterTask /
 * DeleteTask / GetTask 五个方法——把任务定义拼成 XML 一次注册，省掉
 * ITaskDefinition / ITriggerCollection / IActionCollection 那一长串接口声明。
 *
 * GUID 一律由 IIDFromString 从字符串生成，不引用 taskschd.h 里的符号，
 * 换 MinGW 版本或头文件缺失时照样能编译。
 */

#define TASK_NAME           L"WakeGuard"
#define TASK_FLAG_CREATE_OR_UPDATE  6   /* TASK_CREATE_OR_UPDATE */
#define TASK_LOGON_INTERACTIVE_TOKEN 3
#define TASK_ERR_NOT_FOUND  0x80070002L /* HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) */

#define WG_REG_SUBKEY       L"Software\\WakeGuard"

/* VT_EMPTY 的 VARIANT：16 字节全零，按值传递时布局与真实 VARIANT 一致 */
typedef struct { unsigned long long a, b; } WgVariant;

typedef struct WgITaskService WgITaskService;
typedef struct WgITaskFolder  WgITaskFolder;

typedef struct WgIUnknownVtbl {
    HRESULT (WINAPI *QueryInterface)(void *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(void *);
    ULONG   (WINAPI *Release)(void *);
} WgIUnknownVtbl;
typedef struct { WgIUnknownVtbl *lpVtbl; } WgIUnknown;

typedef struct WgITaskServiceVtbl {
    HRESULT (WINAPI *QueryInterface)(WgITaskService *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(WgITaskService *);
    ULONG   (WINAPI *Release)(WgITaskService *);
    HRESULT (WINAPI *GetTypeInfoCount)(WgITaskService *, UINT *);
    HRESULT (WINAPI *GetTypeInfo)(WgITaskService *, UINT, LCID, void **);
    HRESULT (WINAPI *GetIDsOfNames)(WgITaskService *, const GUID *, WCHAR **, UINT, LCID, LONG *);
    HRESULT (WINAPI *Invoke)(WgITaskService *, LONG, const GUID *, LCID, WORD,
                             void *, void *, void *, UINT *);
    HRESULT (WINAPI *GetFolder)(WgITaskService *, WCHAR *, WgITaskFolder **);
    HRESULT (WINAPI *GetRunningTasks)(WgITaskService *, LONG, void **);
    HRESULT (WINAPI *NewTask)(WgITaskService *, DWORD, void **);
    HRESULT (WINAPI *Connect)(WgITaskService *, WgVariant, WgVariant, WgVariant, WgVariant);
    HRESULT (WINAPI *get_Connected)(WgITaskService *, SHORT *);
    HRESULT (WINAPI *get_TargetServer)(WgITaskService *, WCHAR **);
    HRESULT (WINAPI *get_ConnectedUser)(WgITaskService *, WCHAR **);
    HRESULT (WINAPI *get_ConnectedDomain)(WgITaskService *, WCHAR **);
    HRESULT (WINAPI *get_HighestVersion)(WgITaskService *, DWORD *);
} WgITaskServiceVtbl;

struct WgITaskService { WgITaskServiceVtbl *lpVtbl; };

typedef struct WgITaskFolderVtbl {
    HRESULT (WINAPI *QueryInterface)(WgITaskFolder *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(WgITaskFolder *);
    ULONG   (WINAPI *Release)(WgITaskFolder *);
    HRESULT (WINAPI *GetTypeInfoCount)(WgITaskFolder *, UINT *);
    HRESULT (WINAPI *GetTypeInfo)(WgITaskFolder *, UINT, LCID, void **);
    HRESULT (WINAPI *GetIDsOfNames)(WgITaskFolder *, const GUID *, WCHAR **, UINT, LCID, LONG *);
    HRESULT (WINAPI *Invoke)(WgITaskFolder *, LONG, const GUID *, LCID, WORD,
                             void *, void *, void *, UINT *);
    HRESULT (WINAPI *get_Name)(WgITaskFolder *, WCHAR **);
    HRESULT (WINAPI *get_Path)(WgITaskFolder *, WCHAR **);
    HRESULT (WINAPI *GetFolder)(WgITaskFolder *, WCHAR *, WgITaskFolder **);
    HRESULT (WINAPI *GetFolders)(WgITaskFolder *, LONG, void **);
    HRESULT (WINAPI *CreateFolder)(WgITaskFolder *, WCHAR *, WgVariant, WgITaskFolder **);
    HRESULT (WINAPI *DeleteFolder)(WgITaskFolder *, WCHAR *, LONG);
    HRESULT (WINAPI *GetTask)(WgITaskFolder *, WCHAR *, void **);
    HRESULT (WINAPI *GetTasks)(WgITaskFolder *, LONG, void **);
    HRESULT (WINAPI *DeleteTask)(WgITaskFolder *, WCHAR *, LONG);
    HRESULT (WINAPI *RegisterTask)(WgITaskFolder *, WCHAR *, WCHAR *, LONG,
                                   WgVariant, WgVariant, LONG, WgVariant, void **);
    HRESULT (WINAPI *RegisterTaskDefinition)(WgITaskFolder *, WCHAR *, void *, LONG,
                                             WgVariant, WgVariant, LONG, WgVariant, void **);
} WgITaskFolderVtbl;

struct WgITaskFolder { WgITaskFolderVtbl *lpVtbl; };

static const char *TASK_XML_FMT =
"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
"<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
"<RegistrationInfo><Author>WakeGuard</Author>"
"<Description>%s</Description></RegistrationInfo>"
"<Triggers><LogonTrigger>"
"<StartBoundary>2026-01-01T00:00:00</StartBoundary>"
"<Enabled>true</Enabled>"
"<Delay>PT30S</Delay>"
"<UserId>%s</UserId>"
"</LogonTrigger></Triggers>"
"<Principals><Principal id=\"Author\">"
"<LogonType>InteractiveToken</LogonType>"
"<RunLevel>LeastPrivilege</RunLevel>"
"</Principal></Principals>"
"<Settings>"
"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
"<StartWhenAvailable>true</StartWhenAvailable>"
"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
"<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>"
"<Enabled>true</Enabled><Hidden>false</Hidden>"
"</Settings>"
"<Actions Context=\"Author\"><Exec>"
"<Command>&quot;%s&quot;</Command>"
"<WorkingDirectory>%s</WorkingDirectory>"
"</Exec></Actions>"
"</Task>";

/* XML 文本转义：exe 路径里含 & 或引号的情况虽然少见，但不转义会注册失败 */
static void XmlEscape(const char *in, char *out, int n)
{
    int i = 0, k = 0;
    for (; in[i] && k < n - 8; i++) {
        switch (in[i]) {
        case '&':  k += snprintf(out + k, n - k, "&amp;");  break;
        case '<':  k += snprintf(out + k, n - k, "&lt;");   break;
        case '>':  k += snprintf(out + k, n - k, "&gt;");   break;
        case '"':  k += snprintf(out + k, n - k, "&quot;"); break;
        default:   out[k++] = in[i]; break;
        }
    }
    out[k] = 0;
}

static void BuildTaskXml(WCHAR *out, int nChars)
{
    char path[MAX_PATH * 2], dir[MAX_PATH * 2];
    char escPath[MAX_PATH * 4], escDir[MAX_PATH * 4], desc[256], user[256];
    char xml[4096];
    WCHAR wUser[256];
    DWORD len = 255;

    SelfPathUtf8(path, sizeof(path));
    SelfDirUtf8(dir, sizeof(dir));
    XmlEscape(path, escPath, sizeof(escPath));
    XmlEscape(dir, escDir, sizeof(escDir));

    wUser[0] = 0;
    GetUserNameW(wUser, &len);
    WideCharToMultiByte(CP_UTF8, 0, wUser, -1, user, sizeof(user), NULL, NULL);
    XmlEscape(user, desc, sizeof(desc));   /* 用户名同样需要转义 */

    snprintf(xml, sizeof(xml), TASK_XML_FMT, desc, desc, escPath, escDir);
    MultiByteToWideChar(CP_UTF8, 0, xml, -1, out, nChars);
}

/* 取到根任务文件夹；失败返回 NULL。调用方负责 Release svc 与 folder。 */
static WgITaskFolder *TaskGetFolder(WgITaskService **outSvc, int *comOwned)
{
    CLSID clsid; IID iidSvc, iidFolder;
    WgITaskService *svc = NULL;
    WgITaskFolder *folder = NULL;
    WgVariant v;
    HRESULT hr;
    BSTR bRoot;

    memset(&v, 0, sizeof(v));
    *outSvc = NULL;
    *comOwned = 0;

    if (FAILED(IIDFromString(L"{0F87369F-A4E5-4CFC-BD3E-73E6154572DD}", &clsid)) ||
        FAILED(IIDFromString(L"{2FABA4C7-4DA9-4013-9697-20CC3FD40F85}", &iidSvc)) ||
        FAILED(IIDFromString(L"{8CFAC062-A080-4C15-9A88-AA7C2AF80DFC}", &iidFolder))) {
        Log("ERROR", "IIDFromString failed");
        return NULL;
    }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == S_OK) *comOwned = 1;
    else if (FAILED(hr) && hr != (HRESULT)0x80010106L /* RPC_E_CHANGED_MODE */) {
        Log("ERROR", "CoInitializeEx hr=0x%08lX", (unsigned long)hr);
        return NULL;
    }

    hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &iidSvc, (void **)&svc);
    if (FAILED(hr)) {
        Log("ERROR", "TaskScheduler CoCreateInstance hr=0x%08lX", (unsigned long)hr);
        goto fail;
    }
    hr = svc->lpVtbl->Connect(svc, v, v, v, v);
    if (FAILED(hr)) {
        Log("ERROR", "ITaskService::Connect hr=0x%08lX", (unsigned long)hr);
        goto fail;
    }
    bRoot = SysAllocString(L"\\");
    hr = svc->lpVtbl->GetFolder(svc, bRoot, &folder);
    SysFreeString(bRoot);
    if (FAILED(hr)) {
        Log("ERROR", "ITaskService::GetFolder hr=0x%08lX", (unsigned long)hr);
        folder = NULL;
        goto fail;
    }
    *outSvc = svc;
    return folder;

fail:
    if (svc) svc->lpVtbl->Release(svc);
    if (*comOwned) { CoUninitialize(); *comOwned = 0; }
    return NULL;
}

static void TaskRelease(WgITaskService *svc, WgITaskFolder *folder, int comOwned)
{
    if (folder) folder->lpVtbl->Release(folder);
    if (svc) svc->lpVtbl->Release(svc);
    if (comOwned) CoUninitialize();
}

/* 注册或删除计划任务。返回 1 表示达到目标状态。 */
static int SetAutoStartTask(int on)
{
    WgITaskService *svc = NULL;
    WgITaskFolder *folder = NULL;
    int comOwned = 0, ok = 0;
    WgVariant v;
    BSTR bName = NULL, bXml = NULL;
    HRESULT hr;

    memset(&v, 0, sizeof(v));
    folder = TaskGetFolder(&svc, &comOwned);
    if (!folder) return 0;

    bName = SysAllocString(TASK_NAME);
    if (!bName) { Log("ERROR", "SysAllocString(name) failed"); goto done; }

    if (!on) {
        hr = folder->lpVtbl->DeleteTask(folder, bName, 0);
        ok = SUCCEEDED(hr) || hr == (HRESULT)TASK_ERR_NOT_FOUND;
        Log("INFO", "task delete hr=0x%08lX ok=%d", (unsigned long)hr, ok);
    } else {
        void *task = NULL;
        WCHAR xml[4096];
        BuildTaskXml(xml, 4096);
        bXml = SysAllocString(xml);
        if (!bXml) { Log("ERROR", "SysAllocString(xml) failed"); goto done; }
        hr = folder->lpVtbl->RegisterTask(folder, bName, bXml,
                                          TASK_FLAG_CREATE_OR_UPDATE, v, v,
                                          TASK_LOGON_INTERACTIVE_TOKEN, v, &task);
        ok = SUCCEEDED(hr);
        Log("INFO", "task register hr=0x%08lX ok=%d", (unsigned long)hr, ok);
        if (task) ((WgIUnknown *)task)->lpVtbl->Release(task);
    }

done:
    if (bName) SysFreeString(bName);
    if (bXml) SysFreeString(bXml);
    TaskRelease(svc, folder, comOwned);
    return ok;
}

/* 计划任务是否已注册 */
static AutoState AutoStartTaskState(void)
{
    WgITaskService *svc = NULL;
    WgITaskFolder *folder = NULL;
    int comOwned = 0;
    BSTR bName = NULL;
    void *task = NULL;
    HRESULT hr;
    AutoState st = AS_OFF;

    folder = TaskGetFolder(&svc, &comOwned);
    if (!folder) return AS_OFF;

    bName = SysAllocString(TASK_NAME);
    if (!bName) { TaskRelease(svc, folder, comOwned); return AS_OFF; }

    hr = folder->lpVtbl->GetTask(folder, bName, &task);
    if (SUCCEEDED(hr)) st = AS_ON;
    else if (hr != (HRESULT)TASK_ERR_NOT_FOUND)
        Log("WARN", "ITaskFolder::GetTask hr=0x%08lX", (unsigned long)hr);
    if (task) ((WgIUnknown *)task)->lpVtbl->Release(task);

    SysFreeString(bName);
    TaskRelease(svc, folder, comOwned);
    return st;
}

/* 记住注册任务时用的 exe 路径，供启动时做自愈比对 */
static void TaskRememberPath(void)
{
    char path[MAX_PATH * 2];
    SelfPathUtf8(path, sizeof(path));
    RegSetString(HKEY_CURRENT_USER, WG_REG_SUBKEY, L"AutostartTaskPath", path);
}

static void MsgBoxUtf8(const char *text, int isError)
{
    MessageBoxW(g_hwnd, W_(text), L"WakeGuard",
                MB_OK | (isError ? MB_ICONWARNING : MB_ICONINFORMATION) | MB_SETFOREGROUND);
}

/* 必须用 CF_UNICODETEXT：UTF-8 字节塞进 CF_TEXT 会被当成本地代码页而乱码 */
static void CopyToClipboard(const char *utf8)
{
    WCHAR w[2048];
    HGLOBAL h;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 2048);
    if (!OpenClipboard(g_hwnd)) { MsgBoxUtf8("无法打开剪贴板。", 1); return; }
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, (wcslen(w) + 1) * sizeof(WCHAR));
    if (h) {
        WCHAR *p = (WCHAR *)GlobalLock(h);
        lstrcpyW(p, w);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
    MsgBoxUtf8("诊断信息已复制到剪贴板，可直接粘贴。", 0);
}

/* ---------------------------- 自启统一管理（聚合层） ------------------- */

/* 当前真正生效的后端，按 计划任务 > 启动文件夹 > 注册表 取第一个可用的 */
static AutoBackend AutostartActive(void)
{
    if (AutoStartTaskState() == AS_ON) return AST_TASK;
    if (AutoStartFolderOn()  == AS_ON) return AST_FOLDER;
    if (AutoStartOn()        == AS_ON) return AST_REG;
    return AST_NONE;
}

static int AutostartHasStale(void)
{
    return AutoStartTaskState() == AS_STALE ||
           AutoStartFolderOn()  == AS_STALE ||
           AutoStartOn()        == AS_STALE;
}

/* 切换到指定后端：先清掉其余两条，保证任一时刻只有一条生效 */
static int AutostartEnable(AutoBackend b)
{
    if (b != AST_TASK)   SetAutoStartTask(0);
    if (b != AST_FOLDER) SetAutoStartFolder(0);
    if (b != AST_REG)    SetAutoStart(0);

    switch (b) {
    case AST_TASK:
        if (SetAutoStartTask(1)) { TaskRememberPath(); return 1; }
        return 0;
    case AST_FOLDER:
        return SetAutoStartFolder(1);
    case AST_REG:
        return SetAutoStart(1) == 0;
    default:
        return 1;   /* AST_NONE：清完即可 */
    }
}

static int AutostartDisableAll(void)
{
    SetAutoStartTask(0);
    SetAutoStartFolder(0);
    SetAutoStart(0);
    RegDelete(HKEY_CURRENT_USER, WG_REG_SUBKEY, L"AutostartTaskPath");
    return AutostartActive() == AST_NONE;
}

/* 自动选路：按优先级逐条尝试，第一条成功的即为最终方案 */
static AutoBackend AutostartEnableAuto(void)
{
    if (AutostartEnable(AST_TASK))   return AST_TASK;
    if (AutostartEnable(AST_FOLDER)) return AST_FOLDER;
    if (AutostartEnable(AST_REG))    return AST_REG;
    return AST_NONE;
}

/* 启动时自愈：条目还在但它指向的 exe 已不在原地，就按当前 exe 路径重写 */
static void AutostartSelfHeal(void)
{
    char cur[MAX_PATH * 2], saved[512];

    if (AutoStartOn() == AS_STALE) {
        Log("INFO", "self-heal: Run entry points to missing exe, rewriting");
        SetAutoStart(1);
    }
    if (AutoStartFolderOn() == AS_STALE) {
        Log("INFO", "self-heal: shortcut points to missing exe, rewriting");
        SetAutoStartFolder(1);
    }
    if (AutoStartTaskState() == AS_ON) {
        SelfPathUtf8(cur, sizeof(cur));
        if (RegGetString(HKEY_CURRENT_USER, WG_REG_SUBKEY,
                         L"AutostartTaskPath", saved, sizeof(saved)) &&
            strcmp(saved, cur) != 0) {
            Log("INFO", "self-heal: task registered for '%s', re-registering for '%s'",
                saved, cur);
            SetAutoStartTask(1);
            TaskRememberPath();
        }
    }
}

static const char *BackendName(AutoBackend b)
{
    switch (b) {
    case AST_TASK:   return "计划任务";
    case AST_FOLDER: return "启动文件夹";
    case AST_REG:    return "注册表";
    default:         return "未启用";
    }
}

static const char *StateName(AutoState s)
{
    switch (s) {
    case AS_ON:    return "已生效";
    case AS_STALE: return "已失效（目标文件不存在）";
    default:       return "未启用";
    }
}

static void ShowMenu(void)
{
    HMENU m = CreatePopupMenu();
    POINT p;
    int cmd;
    char line[256], stats[256], ver[64];
    const char *blocked = BlockedReason();

    StatusLine(line, sizeof(line));
    snprintf(stats, sizeof(stats), "误报掉电 %d 次 · 息屏 %d 次 · 待机 %d 次",
             g_glitch, g_monitorOff, g_suspend);

    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, W_(line));
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, W_(stats));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_TOGGLE_PAUSE,
                W_(blocked ? "恢复保护" : "暂停保护 30 分钟"));
    AppendMenuW(m, MF_STRING | (Now() < g_allowUntil ? MF_CHECKED : 0),
                ID_ALLOW_ONCE, W_("放行下一次睡眠"));
    AppendMenuW(m, MF_STRING | (ScreenSaverReallyOn() ? MF_CHECKED : 0),
                ID_SCREENSAVER, W_("允许屏幕保护（不建议）"));
    {
        HMENU sub = CreatePopupMenu();
        AutoState stTask = AutoStartTaskState(), stFolder = AutoStartFolderOn(), stReg = AutoStartOn();
        char label[96];
        int allOff = (stTask == AS_OFF && stFolder == AS_OFF && stReg == AS_OFF);

        snprintf(label, sizeof(label), "计划任务（推荐）%s",
                 stTask == AS_STALE ? " · 需修复" : "");
        AppendMenuW(sub, MF_STRING | (stTask == AS_ON ? MF_CHECKED : 0),
                    ID_AUTOSTART_TASK, W_(label));
        snprintf(label, sizeof(label), "启动文件夹%s",
                 stFolder == AS_STALE ? " · 需修复" : "");
        AppendMenuW(sub, MF_STRING | (stFolder == AS_ON ? MF_CHECKED : 0),
                    ID_AUTOSTART_FOLDER, W_(label));
        snprintf(label, sizeof(label), "注册表%s",
                 stReg == AS_STALE ? " · 需修复" : "");
        AppendMenuW(sub, MF_STRING | (stReg == AS_ON ? MF_CHECKED : 0),
                    ID_AUTOSTART, W_(label));
        AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
        AppendMenuW(sub, MF_STRING | (allOff ? MF_CHECKED : 0),
                    ID_AUTOSTART_OFF, W_("关闭开机自启"));
        AppendMenuW(m, MF_STRING | MF_POPUP, (UINT_PTR)sub, W_("开机自启"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_OPENLOG, W_("打开日志目录"));
    AppendMenuW(m, MF_STRING, ID_DIAGNOSE, W_("复制自启动诊断信息"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    snprintf(ver, sizeof(ver), "WakeGuard v%s", WG_VERSION);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, W_(ver));
    AppendMenuW(m, MF_STRING, ID_EXIT, W_("退出"));

    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    cmd = TrackPopupMenu(m, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
                         p.x, p.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
    case ID_TOGGLE_PAUSE:
        if (blocked) { g_pausedUntil = 0; g_allowUntil = 0; Log("INFO", "resumed"); }
        else { g_pausedUntil = Now() + PAUSE_MS; Log("INFO", "paused 30 min"); }
        Apply();
        break;
    case ID_ALLOW_ONCE:
        g_allowUntil = Now() + MANUAL_ALLOW_MS;
        Log("INFO", "allowing next sleep");
        Apply();
        break;
    case ID_SCREENSAVER: {
        int on = !ScreenSaverReallyOn();
        RegSetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop",
                     L"ScreenSaveActive", on ? "1" : "0");
        Log("INFO", "screensaver -> %s", on ? "allowed" : "disabled");
        break;
    }
    case ID_AUTOSTART_TASK:
    case ID_AUTOSTART_FOLDER:
    case ID_AUTOSTART:
    case ID_AUTOSTART_OFF: {
        AutoBackend want;
        char msg[384];
        int ok;
        if (cmd == ID_AUTOSTART_TASK) want = AST_TASK;
        else if (cmd == ID_AUTOSTART_FOLDER) want = AST_FOLDER;
        else if (cmd == ID_AUTOSTART) want = AST_REG;
        else want = AST_NONE;

        ok = (want == AST_NONE) ? AutostartDisableAll() : AutostartEnable(want);
        if (ok) {
            snprintf(msg, sizeof(msg),
                     "开机自启已设为：%s\n"
                     "（其余方式已一并清除，避免重复启动）\n\n"
                     "验证方式：注销后重新登录，而不是重启——\n"
                     "Windows 快速启动下重启不等于完整引导。",
                     BackendName(want));
            MsgBoxUtf8(msg, 0);
        } else {
            snprintf(msg, sizeof(msg),
                     "切换到「%s」失败。\n\n"
                     "常见原因：安全软件拦截，或任务计划程序服务未运行。\n"
                     "可换一种方式再试，详情见「复制自启动诊断信息」。",
                     BackendName(want));
            MsgBoxUtf8(msg, 1);
        }
        break;
    }
    case ID_DIAGNOSE: {
        char text[2048], folder[MAX_PATH], exe[MAX_PATH * 2], errTxt[64];
        AutoState stTask = AutoStartTaskState(), stFolder = AutoStartFolderOn(), stReg = AutoStartOn();
        AutoBackend active = AutostartActive();

        SelfPathUtf8(exe, sizeof(exe));
        StartupFolder(folder, sizeof(folder));
        if (g_lastRegError < 0) snprintf(errTxt, sizeof(errTxt), "本次运行未尝试写入");
        else if (g_lastRegError == 0) snprintf(errTxt, sizeof(errTxt), "0（成功）");
        else if (g_lastRegError == 5) snprintf(errTxt, sizeof(errTxt), "5（访问被拒绝，安全软件拦截）");
        else snprintf(errTxt, sizeof(errTxt), "%ld", g_lastRegError);

        snprintf(text, sizeof(text),
                 "程序路径：%s\n"
                 "\n"
                 "① 计划任务：%s\n"
                 "② 启动文件夹：%s\n"
                 "   位置：%s\n"
                 "③ 注册表 Run 键：%s\n"
                 "   上次写入错误码：%s\n"
                 "\n"
                 "当前生效：%s\n"
                 "托盘图标：%s\n"
                 "建议：%s",
                 exe,
                 StateName(stTask),
                 StateName(stFolder), folder,
                 StateName(stReg), errTxt,
                 BackendName(active),
                 g_trayAdded ? "已显示" : "未显示（Explorer 可能尚未就绪）",
                 AutostartHasStale() ? "有条目已失效，重新点一次对应方式即可按当前路径修复。"
                 : (active == AST_NONE ? "尚未启用开机自启，请在「开机自启」子菜单中选择一种。"
                                       : "已就绪。注销后重新登录验证（重启在快速启动下不算完整引导）。"));
        CopyToClipboard(text);
        Log("INFO", "diagnostics: %s", text);
        break;
    }
    case ID_OPENLOG: {
        WCHAR w[MAX_PATH * 2];
        MultiByteToWideChar(CP_UTF8, 0, g_logDir, -1, w, MAX_PATH * 2);
        ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        break;
    }
    case ID_EXIT:
        Log("INFO", "exit requested");
        PostQuitMessage(0);
        break;
    }
    TrayUpdate();
}

/* ------------------------------------------------------------ 窗口过程 */

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    /* TaskbarCreated 的 ID 由运行时注册，无法写进 case 标签 */
    if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated) {
        TrayReAdd();
        return 0;
    }

    switch (msg) {
    case TRAY_MSG:
        if ((UINT)l == WM_RBUTTONUP) ShowMenu();
        else if ((UINT)l == WM_LBUTTONUP) TrayUpdate();
        return 0;

    case WM_TIMER:
        if (w == TIMER_POLL) Tick();
        else if (w == TIMER_TRAY_RETRY) {
            if (g_trayAdded) { KillTimer(h, TIMER_TRAY_RETRY); return 0; }
            g_trayRetry++;
            Log("INFO", "tray icon retry #%d", g_trayRetry);
            if (TrayAddOnce() || g_trayRetry >= TRAY_RETRY_MAX) {
                KillTimer(h, TIMER_TRAY_RETRY);
                if (!g_trayAdded)
                    Log("ERROR", "tray icon unavailable after %d retries", g_trayRetry);
            }
            return 0;
        }
        else if (g_selfTest) {
            char line[256];
            StatusLine(line, sizeof(line));
            Log("INFO", "SELFTEST holding=%d ac=%d glitch=%d | %s",
                g_holding, g_acOnline, g_glitch, line);
            PostQuitMessage(0);
        }
        return 0;

    case WM_POWERBROADCAST: {
        if (w == PBT_POWERSETTINGCHANGE && l) {
            PBT_SETTING *s = (PBT_SETTING *)l;
            BYTE v = s->Data[0];
            if (memcmp(&s->PowerSetting, &GUID_ACDC, sizeof(GUID)) == 0) {
                g_acOnline = (v == 1);
                if (v == 1) g_lastAcSeen = Now();
            } else if (memcmp(&s->PowerSetting, &GUID_MONITOR, sizeof(GUID)) == 0) {
                if (v == 0) {
                    g_monitorOff++;
                    Log("WARN", "monitor off #%d (holding=%d)", g_monitorOff, g_holding);
                }
            } else if (memcmp(&s->PowerSetting, &GUID_LID, sizeof(GUID)) == 0) {
                g_lidClosed = (v == 0);
                Log("INFO", "lid %s", g_lidClosed ? "closed" : "opened");
            }
            Apply();
            TrayUpdate();
            return TRUE;
        }
        if (w == PBT_APMSUSPEND) {
            g_suspend++;
            Log("WARN", "system suspending #%d", g_suspend);
        } else if (w == PBT_APMRESUMEAUTOMATIC) {
            Log("INFO", "system resumed");
            g_allowUntil = 0;
            Apply();
        }
        return TRUE;
    }

    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_LOCK) { g_locked = 1; Log("INFO", "session locked"); }
        else if (w == WTS_SESSION_UNLOCK) { g_locked = 0; Log("INFO", "session unlocked"); }
        else return 0;
        Apply();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ---------------------------------------------------------------- 入口 */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    int argc = 0, selfTestSec = 10, i;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    HMODULE k32, u32, wts;
    (void)hPrev; (void)cmdLine; (void)show;

    LogOpen();

    for (i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--selftest") == 0) {
            g_selfTest = 1;
            if (i + 1 < argc) selfTestSec = _wtoi(argv[i + 1]);
            if (selfTestSec <= 0) selfTestSec = 10;
        } else if (wcscmp(argv[i], L"--version") == 0) {
            Log("INFO", "WakeGuard v%s (C/Win32)", WG_VERSION);
            return 0;
        } else if (wcscmp(argv[i], L"--autostart") == 0 && i + 1 < argc) {
            const WCHAR *a = argv[i + 1];
            if (wcscmp(a, L"task") == 0) {
                int ok = AutostartEnable(AST_TASK);
                Log("INFO", "autostart task -> %d (now %d)", ok, AutoStartTaskState());
            } else if (wcscmp(a, L"folder") == 0) {
                int ok = AutostartEnable(AST_FOLDER);
                Log("INFO", "autostart folder -> %d (now %d)", ok, AutoStartFolderOn());
            } else if (wcscmp(a, L"auto") == 0) {
                AutoBackend b = AutostartEnableAuto();
                Log("INFO", "autostart auto -> %s", BackendName(b));
            } else if (wcscmp(a, L"off") == 0 || wcscmp(a, L"folderoff") == 0) {
                AutostartDisableAll();
                Log("INFO", "autostart off (now %s)", BackendName(AutostartActive()));
            } else if (wcscmp(a, L"status") == 0) {
                Log("INFO", "autostart status: task=%d folder=%d reg=%d active=%s",
                    AutoStartTaskState(), AutoStartFolderOn(), AutoStartOn(),
                    BackendName(AutostartActive()));
            } else {
                int ok = AutostartEnable(AST_REG);        /* on / reg */
                Log("INFO", "autostart reg -> %d (now %d)", ok, AutoStartOn());
            }
            return 0;
        }
    }
    LocalFree(argv);

    /* 动态解析可能不在旧导入库里的 API */
    k32  = GetModuleHandleW(L"kernel32.dll");
    u32  = GetModuleHandleW(L"user32.dll");
    pPowerCreateRequest = (PFN_PCR)GetProcAddress(k32, "PowerCreateRequest");
    pPowerSetRequest    = (PFN_PSR)GetProcAddress(k32, "PowerSetRequest");
    pPowerClearRequest  = (PFN_PSR)GetProcAddress(k32, "PowerClearRequest");
    pRegPowerSetting    = (PFN_RPSN)GetProcAddress(u32, "RegisterPowerSettingNotification");
    pUnregPowerSetting  = (PFN_UPSN)GetProcAddress(u32, "UnregisterPowerSettingNotification");
    pCreateIconFromResourceEx = (PFN_CIFRE)GetProcAddress(u32, "CreateIconFromResourceEx");
    wts = LoadLibraryW(L"wtsapi32.dll");
    if (wts) {
        pWTSRegister   = (PFN_WTSREG)GetProcAddress(wts, "WTSRegisterSessionNotification");
        pWTSUnRegister = (PFN_WTSUNREG)GetProcAddress(wts, "WTSUnRegisterSessionNotification");
    }
    if (!pPowerCreateRequest || !pPowerSetRequest) {
        Log("ERROR", "PowerRequest API unavailable");
        MessageBoxW(NULL, L"PowerRequest API unavailable (requires Windows 7+).",
                    L"WakeGuard", MB_ICONERROR);
        return 2;
    }

    g_mutex = CreateMutexW(NULL, TRUE, L"Local\\WakeGuard_Singleton_Mutex");
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("WARN", "already running, exiting");
        CloseHandle(g_mutex);
        return 1;
    }

    Log("INFO", "==== WakeGuard v%s (C/Win32) start ====", WG_VERSION);

    g_iconActive = MakeIcon(0x1D, 0x9E, 0x75);
    g_iconIdle   = MakeIcon(0x88, 0x87, 0x80);
    g_iconPaused = MakeIcon(0xEF, 0x9F, 0x27);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"WakeGuardMsgWindow";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"WakeGuardMsgWindow", L"WakeGuard", 0,
                             0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwnd) { Log("ERROR", "CreateWindowExW failed (%lu)", GetLastError()); return 2; }
    Log("INFO", "message window created");

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (pRegPowerSetting) {
        g_notify[0] = pRegPowerSetting(g_hwnd, &GUID_ACDC, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_notify[1] = pRegPowerSetting(g_hwnd, &GUID_MONITOR, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_notify[2] = pRegPowerSetting(g_hwnd, &GUID_LID, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    if (pWTSRegister) pWTSRegister(g_hwnd, 0);

    SetTimer(g_hwnd, TIMER_POLL, POLL_MS, NULL);
    Tick();

    if (g_selfTest) {
        SetTimer(g_hwnd, TIMER_SELFTEST, (UINT)(selfTestSec * 1000), NULL);
    } else {
        TrayAdd();
        AutostartSelfHeal();   /* 条目指向的 exe 不在原地时按当前路径重写 */
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Release("shutting down");
    if (g_req) CloseHandle(g_req);
    TrayRemove();
    for (i = 0; i < 3; i++) if (g_notify[i]) pUnregPowerSetting(g_notify[i]);
    if (pWTSUnRegister && g_hwnd) pWTSUnRegister(g_hwnd);
    if (g_iconActive) DestroyIcon(g_iconActive);
    if (g_iconIdle) DestroyIcon(g_iconIdle);
    if (g_iconPaused) DestroyIcon(g_iconPaused);
    if (g_mutex) CloseHandle(g_mutex);
    Log("INFO", "exited");
    if (g_log) fclose(g_log);
    return 0;
}
