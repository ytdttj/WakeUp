# WakeGuard

> 为 KB5121003 受害者研发的防息屏工具。
> 缓解 Windows 11 `KB5121003`（OS Build 26200.9168）电源回归导致的随机息屏。
> 纯 C / Win32 单文件实现，约 87 KB，常驻私有内存约 2.5 MB，无运行时依赖。

---

## 背景

微软 2026 年 8 月安全更新 `KB5121003` 引入了一个电源回归：部分笔记本（联想 Legion / Y9000P 等机型报告最多）
会**错误地把交流适配器判定为断开 1~5 秒**，随后又恢复。

在支持 **Modern Standby（S0ix）** 的机器上，这个误报会立刻触发直流侧的电源策略：屏幕熄灭，系统滑入
连接待机（事件查看器里表现为 `Kernel-Power 506` 进入 / `507` 退出），1 秒后再退出——用户看到的就是
"时不时就黑一下"。

事件日志特征：

| 事件 | 含义 |
|---|---|
| `Kernel-Power 105` | 电源状态变更，`AcOnline=false` 后紧跟 `=true`，间隔约 1 秒 |
| `Kernel-Power 506 / 507` | Modern Standby 进入 / 退出（= 真正发生的"息屏"） |
| `Power-Troubleshooter 1` | 传统睡眠唤醒记录（此问题下通常**没有**） |

微软与联想均未在官方已知问题列表中承认该问题，且该补丁会随后续累积更新固化，**无法单独卸载**。

## 为什么不是"检测到休眠再唤醒"

这是最直觉的思路，但在用户态做不到，原因有三：

1. 自 Windows Vista 起，用户态**无法否决已进入的睡眠转换**。收到 `PBT_APMSUSPEND` 时为时已晚，
   只有内核驱动能拦。
2. Modern Standby 进入 DRIPS 后，桌面活动调节器（DAM）会**节流普通用户态进程**，定时器不可靠。
3. 实测这类机器的电源计划中，**直流侧"允许使用唤醒定时器"通常是禁用的**——而这个 bug 恰恰让系统
   以为自己处于直流供电，等于把唤醒定时器这条路也堵死了。

所以本项目采用**预防式**策略：不让系统走到息屏那一步。

## 原理

1. **持续持有电源请求**：`PowerCreateRequest` + `PowerSetRequest(PowerRequestDisplayRequired |
   PowerRequestSystemRequired)`，让系统既不关闭显示器，也不进入 Modern Standby。
2. **AC 去抖（sticky 60 秒）**：每 500 ms 轮询 `GetSystemPowerStatus`，只要最近 60 秒内出现过
   AC 在线就判定为物理接电。1~5 秒的误报被完全吸收；真正拔掉电源 60 秒后自动交还系统策略。
3. **尊重用户主动操作**：锁屏、合盖、菜单「放行下一次睡眠」都会立即释放电源请求。
4. **事件驱动 + 单线程**：一个隐藏消息窗口 + `WM_TIMER(500ms)`，配合
   `RegisterPowerSettingNotification` 与 `WTSRegisterSessionNotification`，CPU 占用接近 0。

## 编译

需要 MinGW GCC（Windows 上 Strawberry Perl 自带的即可，无需额外安装）：

```bat
gcc -O2 -s -mwindows -fexec-charset=UTF-8 -finput-charset=UTF-8 ^
    -o bin\WakeGuardC.exe src\wakeguard.c ^
    -luser32 -lshell32 -ladvapi32 -lgdi32 -lole32 -luuid
```

`-fexec-charset=UTF-8` 是必需的：源码中的中文以 UTF-8 窄字符串存放，运行时再经
`MultiByteToWideChar(CP_UTF8)` 转成宽字符交给 Win32 API。

`PowerCreateRequest`、`RegisterPowerSettingNotification`、`WTSRegisterSessionNotification` 等
可能不在旧导入库中的 API 一律用 `GetProcAddress` 动态解析，因此不挑 MinGW 版本。

## 使用

双击 `bin\WakeGuardC.exe`，托盘出现闪电图标：

| 颜色 | 含义 |
|---|---|
| 绿色 | 保护中（判定为接电） |
| 橙色 | 已暂停 / 已锁屏 / 已合盖 / 已放行 |
| 灰色 | 未接电，已交还系统策略 |

鼠标悬停显示两行：

```
保护中 · 电源接入 · 电量 97%
误报掉电 3 次 · 息屏 0 次 · 待机 0 次
```

右键菜单：暂停保护 30 分钟、放行下一次睡眠、允许屏幕保护、开机自启动（注册表 / 启动文件夹）、
打开日志目录、复制自启动诊断信息、退出。

`start_wakeguard.vbs` / `stop_wakeguard.vbs` 提供无窗口启停；`update.bat` 用于在运行中替换可执行文件。

## 开机自启动

提供两条独立路径，菜单中分别显示勾选状态：

- **注册表**：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
- **启动文件夹**：在 `shell:startup` 放置快捷方式（**推荐**，可绕开安全软件对 Run 键的拦截）

也可命令行操作：`WakeGuardC.exe --autostart on|off|folder|folderoff`

## 诊断

日志文件：`%LOCALAPPDATA%\WakeGuard\wakeguard.log`

命令行开关：

```
WakeGuardC.exe --selftest 10    # 以自检模式运行 10 秒后退出（不显示托盘）
WakeGuardC.exe --version
WakeGuardC.exe --autostart on
```

若「息屏」计数持续增加，说明存在电源请求覆盖不到的息屏来源（例如厂商自带电源管理软件的独立策略），
需要另行排查。

## 实测占用

| 实现 | 体积 | 私有内存 | 线程 |
|---|---|---|---|
| C/Win32（本项目） | 87 KB | 2.54 MB | 6 |
| C# 单文件自包含 | 10.7 MB | 11.1 MB | 8 |
| Python + pystray | 27 KB + 解释器 | 40~60 MB | 5+ |

## 文件说明

```
src/wakeguard.c        C/Win32 实现（唯一维护的源码）
bin/WakeGuardC.exe     编译产物
start_wakeguard.vbs    无窗口启动
stop_wakeguard.vbs     结束进程
update.bat             运行中替换 exe（taskkill → copy → start）
```

其它语言实现（Python / C#）与构建产物已由 `.gitignore` 排除，仅作本地备份，不再维护。

## 已知限制

- 保护期间**屏幕不会自动关闭**，这是"阻止息屏"的必然代价；需要临时交还系统策略请用
  「暂停保护 30 分钟」或锁屏。
- AC 去抖窗口固定为 60 秒（源码中 `AC_STICKY_MS` 可调整）。
- 仅缓解误报触发的息屏，不修复 `KB5121003` 本身。
