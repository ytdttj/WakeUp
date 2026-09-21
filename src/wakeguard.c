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
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ---------------------------------------------------------------- 常量 */

#define WG_VERSION          "1.1"

#define AC_STICKY_MS        60000
#define POLL_MS             500
#define MANUAL_ALLOW_MS     300000
#define PAUSE_MS            (30 * 60 * 1000)

#define TRAY_MSG            (WM_USER + 1)
#define TIMER_POLL          1
#define TIMER_SELFTEST      2

#define ID_TOGGLE_PAUSE     1001
#define ID_ALLOW_ONCE       1002
#define ID_SCREENSAVER      1003
#define ID_AUTOSTART        1004
#define ID_OPENLOG          1005
#define ID_EXIT             1006
#define ID_AUTOSTART_FOLDER 1007
#define ID_DIAGNOSE         1008

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
static HANDLE g_mutex = NULL;
static HANDLE g_notify[3] = {NULL, NULL, NULL};
static FILE  *g_log = NULL;
static char   g_logDir[MAX_PATH] = {0};
static char   g_logPath[MAX_PATH] = {0};

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
    char l1[128], l2[128], buf[256];
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

static void TrayAdd(void)
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
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) ? 1 : 0;
    if (g_trayAdded) {
        lstrcpyW(g_lastTip, nid.szTip);
        g_lastIcon = nid.hIcon;
    }
    Log("INFO", g_trayAdded ? "tray icon added" : "Shell_NotifyIconW failed");
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

static int AutoStartOn(void)
{
    char v[512] = {0};
    RegGetString(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                 L"WakeGuard", v, sizeof(v));
    return v[0] != 0;
}

/* 返回 0 成功，非 0 为 Win32 错误码 */
static LONG SetAutoStart(int on)
{
    const WCHAR *sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    LONG rc;
    if (on) {
        WCHAR path[MAX_PATH];
        char cmd[MAX_PATH * 2], quoted[MAX_PATH * 2 + 4];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, cmd, sizeof(cmd), NULL, NULL);
        snprintf(quoted, sizeof(quoted), "\"%s\"", cmd);
        rc = RegSetString(HKEY_CURRENT_USER, sub, L"WakeGuard", quoted);
        g_lastRegError = rc;
        Log("INFO", "reg autostart set rc=%ld value=%s", rc, quoted);
    } else {
        rc = RegDeleteEx(HKEY_CURRENT_USER, sub, L"WakeGuard");
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

static int CreateShortcut(const WCHAR *target, const WCHAR *linkPath, const WCHAR *desc)
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
    if (!CreateShortcut(wTarget, wLink, L"WakeGuard")) {
        Log("ERROR", "CreateShortcut failed");
        return 0;
    }
    MultiByteToWideChar(CP_UTF8, 0, folder, -1, wFolder, MAX_PATH);
    attr = GetFileAttributesW(wLink);
    Log("INFO", "startup folder link created attr=0x%lX", (unsigned long)attr);
    return attr != INVALID_FILE_ATTRIBUTES;
}

static int AutoStartFolderOn(void)
{
    char folder[MAX_PATH], linkPath[MAX_PATH * 2];
    WCHAR wLink[MAX_PATH];
    StartupFolder(folder, sizeof(folder));
    if (folder[0] == 0) return 0;
    snprintf(linkPath, sizeof(linkPath), "%s\\WakeGuard.lnk", folder);
    MultiByteToWideChar(CP_UTF8, 0, linkPath, -1, wLink, MAX_PATH);
    return GetFileAttributesW(wLink) != INVALID_FILE_ATTRIBUTES;
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
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "诊断信息已复制到剪贴板：\n\n%s", utf8);
        MsgBoxUtf8(msg, 0);
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
    AppendMenuW(m, MF_STRING | (AutoStartOn() ? MF_CHECKED : 0),
                ID_AUTOSTART, W_("开机自启动（注册表）"));
    AppendMenuW(m, MF_STRING | (AutoStartFolderOn() ? MF_CHECKED : 0),
                ID_AUTOSTART_FOLDER, W_("开机自启动（启动文件夹）"));
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
    case ID_AUTOSTART: {
        int wantOn = !AutoStartOn();
        LONG rc = SetAutoStart(wantOn);
        char msg[384];
        if (rc == 0 && AutoStartOn() == wantOn)
            snprintf(msg, sizeof(msg), "注册表自启动已%s。\n如果联想电脑管家没有弹窗确认，"
                                       "试试下面那一项「开机自启动（启动文件夹）」。",
                     wantOn ? "开启" : "关闭");
        else
            snprintf(msg, sizeof(msg), "注册表写入失败：错误码 %ld\n"
                                       "（5=被拒绝，可能需要管家/安全软件放行）\n"
                                       "可改用「开机自启动（启动文件夹）」。", rc);
        MsgBoxUtf8(msg, rc != 0);
        break;
    }
    case ID_AUTOSTART_FOLDER: {
        int wantOn = !AutoStartFolderOn();
        int ok = SetAutoStartFolder(wantOn);
        char msg[320], folder[MAX_PATH];
        StartupFolder(folder, sizeof(folder));
        if (ok && AutoStartFolderOn() == wantOn) {
            snprintf(msg, sizeof(msg), "启动文件夹快捷方式已%s。\n位置：%s",
                     wantOn ? "创建" : "删除", folder);
            MsgBoxUtf8(msg, 0);
        } else {
            snprintf(msg, sizeof(msg), "操作失败。\n启动文件夹：%s", folder);
            MsgBoxUtf8(msg, 1);
        }
        break;
    }
    case ID_DIAGNOSE: {
        char text[1024], folder[MAX_PATH], exe[MAX_PATH * 2], errTxt[64];
        WCHAR wExe[MAX_PATH];
        GetModuleFileNameW(NULL, wExe, MAX_PATH);
        WideCharToMultiByte(CP_UTF8, 0, wExe, -1, exe, sizeof(exe), NULL, NULL);
        StartupFolder(folder, sizeof(folder));
        if (g_lastRegError < 0) snprintf(errTxt, sizeof(errTxt), "本次运行未尝试写入");
        else if (g_lastRegError == 0) snprintf(errTxt, sizeof(errTxt), "0（成功）");
        else if (g_lastRegError == 5) snprintf(errTxt, sizeof(errTxt), "5（访问被拒绝，安全软件拦截）");
        else snprintf(errTxt, sizeof(errTxt), "%ld", g_lastRegError);
        snprintf(text, sizeof(text),
                 "程序路径：%s\n"
                 "① 注册表自启动：%s\n"
                 "② 注册表上次写入错误码：%s\n"
                 "③ 启动文件夹：%s\n"
                 "④ 启动文件夹快捷方式：%s\n"
                 "建议：%s",
                 exe,
                 AutoStartOn() ? "已启用" : "未启用",
                 errTxt,
                 folder,
                 AutoStartFolderOn() ? "已创建" : "不存在",
                 AutoStartFolderOn() ? "快捷方式已就绪，重启即可验证。"
                                     : "请点「开机自启动（启动文件夹）」。");
        CopyToClipboard(text);
        Log("INFO", "diagnostics: %s", text);
        break;
    }
    case ID_OPENLOG: {
        WCHAR w[512];
        MultiByteToWideChar(CP_UTF8, 0, g_logDir, -1, w, 512);
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
    switch (msg) {
    case TRAY_MSG:
        if ((UINT)l == WM_RBUTTONUP) ShowMenu();
        else if ((UINT)l == WM_LBUTTONUP) TrayUpdate();
        return 0;

    case WM_TIMER:
        if (w == TIMER_POLL) Tick();
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
            if (wcscmp(argv[i + 1], L"folder") == 0) {
                int ok = SetAutoStartFolder(1);           /* 先执行 */
                Log("INFO", "folder autostart -> %d (now %d)", ok, AutoStartFolderOn());
            } else if (wcscmp(argv[i + 1], L"folderoff") == 0) {
                int ok = SetAutoStartFolder(0);
                Log("INFO", "folder autostart off -> %d (now %d)", ok, AutoStartFolderOn());
            } else {
                SetAutoStart(wcscmp(argv[i + 1], L"on") == 0);
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

    if (pRegPowerSetting) {
        g_notify[0] = pRegPowerSetting(g_hwnd, &GUID_ACDC, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_notify[1] = pRegPowerSetting(g_hwnd, &GUID_MONITOR, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_notify[2] = pRegPowerSetting(g_hwnd, &GUID_LID, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    if (pWTSRegister) pWTSRegister(g_hwnd, 0);

    SetTimer(g_hwnd, TIMER_POLL, POLL_MS, NULL);
    Tick();

    if (g_selfTest) SetTimer(g_hwnd, TIMER_SELFTEST, (UINT)(selfTestSec * 1000), NULL);
    else TrayAdd();

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
