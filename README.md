# WakeGuard

> 为 KB5121003 受害者研发的防息屏工具。
> 缓解 Windows 11 `KB5121003`（OS Build 26200.9168）电源回归导致的随机息屏。
> 纯 C / Win32 单文件实现，无运行时依赖。

---

## 背景

微软 2026 年 8 月安全更新 `KB5121003` 引入了一个电源回归：部分笔记本（联想 Legion / Y9000P 等机型报告最多）
会**错误地把交流适配器判定为断开 1~5 秒**，随后又恢复。

误报本身只有一瞬间，但它会触发一个连锁反应：系统一旦认为自己"在用电池"，就会从交流电源计划切换到
**直流（电池）电源计划**，而电池计划里的「关闭屏幕」超时通常设得很短——例如本机的设置是
**用电池时闲置 3 分钟关闭屏幕**。

关键在于系统比较的不是"还剩多久"，而是**当前已经闲置了多久**。插着电时屏幕超时往往设得很长（或从不），
你可能已经离开了几分钟；误报发生的那一刻系统切到电池计划，发现累计空闲时间早已超过 3 分钟，
于是**立刻关闭屏幕**。

于是出现了这个反直觉的现象：

> 电源一直插着，只是在桌前没动一会儿，屏幕就自己黑了。

事件日志特征：

| 事件 | 含义 |
|---|---|
| `Kernel-Power 105` | 电源状态变更，`AcOnline=false` 后紧跟 `=true`，间隔 1~5 秒（误报本身） |
| `Kernel-Power 506 / 507` | Modern Standby 进入 / 退出；本问题下通常**没有**——系统只是关了屏幕，并没有待机 |
| `Power-Troubleshooter 1` | 传统睡眠唤醒记录；本问题下通常**没有** |

微软与联想均未在官方已知问题列表中承认该问题，且该补丁会随后续累积更新固化，**无法单独卸载**。

## 为什么不是"检测到息屏再唤醒"

这是最直觉的思路，但在用户态做不到，原因有三：

1. 自 Windows Vista 起，用户态**无法否决已进入的睡眠转换**。收到 `PBT_APMSUSPEND` 时为时已晚，
   只有内核驱动能拦。
2. Modern Standby 进入 DRIPS 后，桌面活动调节器（DAM）会**节流普通用户态进程**，定时器不可靠。
3. 实测这类机器的电源计划中，**直流侧"允许使用唤醒定时器"通常是禁用的**——而这个 bug 恰恰让系统
   以为自己处于直流供电，等于把唤醒定时器这条路也堵死了。

所以本项目采用**预防式**策略：不让系统走到息屏那一步。

## 原理

1. **持续持有电源请求**：`PowerCreateRequest` + `PowerSetRequest(PowerRequestDisplayRequired |
   PowerRequestSystemRequired)`。其中 `DisplayRequired` 会**抑制屏幕超时**——哪怕电池策略写着
   "闲置 3 分钟关闭屏幕"，只要这个请求还在，屏幕就不会关。这正是本问题的解药。
2. **AC 去抖（sticky 60 秒）**：每 500 ms 轮询 `GetSystemPowerStatus`，只要最近 60 秒内出现过
   AC 在线就判定为物理接电。1~5 秒的误报被完全吸收，系统始终拿不到"用电池"这个前提，
   也就不会切到那一套短超时策略；真正拔掉电源 60 秒后自动交还系统策略。
3. **尊重用户主动操作**：锁屏、合盖、菜单「放行下一次睡眠」都会立即释放电源请求。
4. **事件驱动 + 单线程**：一个隐藏消息窗口 + `WM_TIMER(500ms)`，配合
   `RegisterPowerSettingNotification` 与 `WTSRegisterSessionNotification`，CPU 占用接近 0。

## 编译

需要 MinGW GCC（Windows 上 Strawberry Perl 自带的即可，无需额外安装）：

```bat
gcc -O2 -s -mwindows -fexec-charset=UTF-8 -finput-charset=UTF-8 ^
    -o bin\WakeGuardC.exe src\wakeguard.c ^
    -luser32 -lshell32 -ladvapi32 -lgdi32 -lole32 -loleaut32 -luuid
```

`-fexec-charset=UTF-8` 是必需的：源码中的中文以 UTF-8 窄字符串存放，运行时再经
`MultiByteToWideChar(CP_UTF8)` 转成宽字符交给 Win32 API。

`PowerCreateRequest`、`RegisterPowerSettingNotification`、`WTSRegisterSessionNotification` 等
可能不在旧导入库中的 API 一律用 `GetProcAddress` 动态解析，因此不挑 MinGW 版本。

任务计划程序相关的 COM 接口为手写最小 vtable，GUID 一律用 `IIDFromString` 从字符串生成，
**不引用 `taskschd.h` 的任何符号**，换 MinGW 版本也不受影响。

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

右键菜单：暂停保护 30 分钟、放行下一次睡眠、允许屏幕保护、开机自启（子菜单，三种方式）、
打开日志目录、复制自启动诊断信息、退出。

`update.bat` 用于在程序运行时替换 exe——exe 正在运行会被占用、无法直接覆盖，
脚本会先停进程、再替换、再重启。

## 开机自启动

提供三种方式，收在同一个子菜单里，**任一时刻只有一种生效**（切换时会自动清掉其余两种）：

| 方式 | 触发时机 | 特点 |
|---|---|---|
| **计划任务**（推荐） | 用户登录时，再延迟 30 秒 | 最可靠；不被安全软件针对；延迟正好避开 Explorer 托盘初始化的竞态 |
| 启动文件夹 | 桌面就绪后 | 杀软基本不拦、用户可见可管理；exe 换目录后快捷方式会失效 |
| 注册表 `HKCU\...\Run` | 桌面就绪后 | 实现最简单；**第三方管家/杀软高概率拦截或静默删除** |

计划任务的关键设置：`LogonType=InteractiveToken`（必须在用户会话内运行，Session 0 没有托盘）、
`RunLevel=LeastPrivilege`（不需要提权，避免 UAC 弹窗）、`DisallowStartIfOnBatteries=false`
（笔记本用电池时照样要启动）、`MultipleInstancesPolicy=IgnoreNew`（与单实例互斥体一致）。

命令行：

```
WakeGuardC.exe --autostart task      # 计划任务
WakeGuardC.exe --autostart folder    # 启动文件夹
WakeGuardC.exe --autostart on        # 注册表 Run 键
WakeGuardC.exe --autostart auto      # 按上面顺序自动挑一个能成功的
WakeGuardC.exe --autostart off       # 全部关闭
WakeGuardC.exe --autostart status    # 只看状态，不改动
```

状态是三态的：未启用 / 已生效 / **已失效**（条目还在，但它指向的 exe 已经不在那个路径了）。
最后一种在菜单里标为「需修复」，重新点一次对应方式即可按当前路径重写；程序每次启动也会自动自愈一次。

> **验证自启是否生效，要注销后重新登录，而不是重启。** Windows 默认开启快速启动，
> "关机→开机"走的是休眠恢复路径，重启不等于完整引导。

## 诊断

日志文件：`%LOCALAPPDATA%\WakeGuard\wakeguard.log`

命令行开关：

```
WakeGuardC.exe --selftest 10    # 以自检模式运行 10 秒后退出（不显示托盘）
WakeGuardC.exe --version
WakeGuardC.exe --autostart status
```

若「息屏」计数持续增加，说明存在电源请求覆盖不到的息屏来源（例如厂商自带电源管理软件的独立策略），
需要另行排查。

## 已知限制

- 保护期间**屏幕不会自动关闭**，这是"阻止息屏"的必然代价；需要临时交还系统策略请用
  「暂停保护 30 分钟」或锁屏。
- AC 去抖窗口固定为 60 秒（源码中 `AC_STICKY_MS` 可调整）。
- 仅缓解误报触发的息屏，不修复 `KB5121003` 本身。
- 开机自启若选了启动文件夹或注册表方式，程序是在桌面就绪之后才被拉起的，在此之前的一小段时间不受保护。
  选计划任务可以把这个窗口缩到最短（登录后 30 秒）。
