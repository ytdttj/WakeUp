# WakeGuard 开机自启改进规划

> 状态：**已实施**（2026-09-23，v1.2）。下方正文保留为决策记录。
> 起草日期：2026-09-23
> 基线：`src/wakeguard.c`（900 行，v1.1）

> 实施备注：`IID_ITaskFolder` 的实际值是 `8CFAC062-A080-4C15-9A88-AA7C2AF80DFC`，
> 起草时凭记忆写的 `…AA7C05AFDA9C` 是错的，已按 `taskschd.h` 核对修正。
> MinGW（Strawberry 自带）实际**带有** `taskschd.h` 与 `libtaskschd.a`，
> 但源码仍走 `IIDFromString` 路线，保持换编译器也能编译。

---

## 1. 现状盘点

### 1.1 已实现的自启路径

| 路径 | 位置 | 写入方式 | 检测方式 |
|---|---|---|---|
| 注册表 Run 键 | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WakeGuard` | `SetAutoStart()` L457 | `AutoStartOn()` L448 |
| 启动文件夹 | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\WakeGuard.lnk` | `SetAutoStartFolder()` L515 | `AutoStartFolderOn()` L551 |

命令行：`WakeGuardC.exe --autostart on|off|folder|folderoff`（L808）

两条路径在托盘菜单里各自独立打勾（L612 / L614），互不感知。

### 1.2 发现的问题

**P0-1　托盘图标一次性添加，无自愈**

`Shell_NotifyIconW(NIM_ADD)` 全程只在 `TrayAdd()` 调用一次，源码中**不存在** `TaskbarCreated` 消息处理。

自启场景是个真实竞态：Run 键与启动文件夹都在 Explorer shell 就绪之后才被拉起，但"Explorer 进程存在"与"托盘区窗口已创建完成"是两件事。若 exe 早于托盘就绪时机执行，`NIM_ADD` 会静默失败——进程正常常驻、电源请求正常持有，但**托盘图标不存在**，用户既看不到状态也退不出来。同理，Explorer 崩溃自重启后图标永久丢失。

**P0-2　自启状态检测只看"值是否存在"，不校验有效性**

`AutoStartOn()` 的实现是取出注册表值后判断 `v[0] != 0`。只要值非空就返回"已启用"。

后果：exe 被移动、改名、删除后，菜单仍显示勾选，实际每次登录都会失败。这属于"静默失效"，用户往往在重装系统或换目录后很久才发现。

**P0-3　快捷方式缺工作目录与图标**

`CreateShortcut()`（L489）只设置了 `SetPath` 和 `SetDescription`，没有 `SetWorkingDirectory`，也没有 `SetIconLocation`。虽然当前实现不依赖工作目录（日志写在 `%LOCALAPPDATA%`），但一旦将来引入同目录的相对路径资源就会踩坑；托盘/启动文件夹里显示默认图标也影响辨识度。

**P0-4　辅助脚本硬编码路径**

`start_wakeguard.vbs` 写死 `D:\WorkBuddyProjects\WakeUp\bin\WakeGuardC.exe`；`update.bat` 写死待替换文件名 `WakeGuardC_v11.exe`。项目一旦换目录或升版本就得手工改脚本。

**P1-1　缺 Task Scheduler 这条主力路径**

目前只覆盖了"桌面就绪后"这一类方案，缺一块最可靠的拼图。详见第 2 节。

**P2-1　两条路径可同时启用**

现有设计允许 Run 键与启动文件夹同时打勾。虽然 `Local\WakeGuard_Singleton_Mutex`（L844）能保证不会跑两份进程，但"两个开关 + 一个互斥体"的组合让用户难以判断当前到底靠哪个生效，诊断信息也需要冗余解释。

---

## 2. 方案调研结论

### 2.1 Windows 自启六条路径对比

| 方案 | 触发时机 | 权限 | 实测延迟 | 优点 | 缺点 |
|---|---|---|---|---|---|
| Windows 服务 | 内核就绪后 | SYSTEM | ≈13s | 最早、可自动重启 | **Session 0 隔离，无法显示托盘，本项目不可用** |
| 计划任务 · 系统启动 | 内核就绪后 | 可提权 | ≈14s | 早、可延迟 | 同上，需要用户会话 GUI 时不适用 |
| **计划任务 · 用户登录** | 登录瞬间 | 用户级 | **≈19s** | 可靠性最高、可配延迟/失败重试、不受启动文件夹被清理影响 | 需要 COM 或 schtasks 创建，实现量最大 |
| `HKLM\Run` | 桌面就绪后 | 需管理员 | ≈52s | 全用户生效 | 提权才能写，普通用户安装无法使用 |
| `HKCU\Run`（现有） | 桌面就绪后 | 用户级 | ≈79s | 实现最简单 | **第三方杀软/管家软件高概率拦截或静默删除** |
| 启动文件夹（现有） | 桌面就绪后 | 用户级 | ≈81s | 杀软基本不拦、用户可见可管理 | exe 移动后快捷方式失效；用户可能误删 |

> 延迟数据来自社区在 SSD 机型上的实测（开机到程序实际执行），绝对值随机器差异很大，**仅用于横向比较触发时机**。

### 2.2 为什么本项目该补计划任务

1. **时机场次更好**。≈19s 登录瞬间触发，比现在快约 60 秒——意味着保护更早生效，覆盖"登录到桌面就绪"这段窗口。
2. **可配置延迟**。`LogonTrigger/Delay` 支持 `PT30S` 之类的延迟，正好用来规避 P0-1 的 Explorer 竞态，双保险。
3. **最不容易失效**。既不被安全软件针对，也不会像快捷方式那样因移动目录而断链；且计划任务登记的仍是 exe 绝对路径，配合第 4 节的自愈机制一致性最好。
4. **可选失败重试**。`Settings/RestartOnFailure` 能在进程异常退出后自动重启，对一个"防息屏常驻工具"是有意义的兜底。

> **注意：**「最高权限运行」对本工具**不需要**。WakeGuard 不写受保护目录、不提权，用标准用户令牌即可；勾 `HighestAvailable` 反而会触发 UAC 弹窗，违背"开机无感启动"的目标。

---

## 3. 目标架构

```
托盘菜单 / 命令行  --autostart <mode>
        │
        ▼
自启状态聚合层   AutostartProbe()  →  { 未启用 | 已生效 | 僵尸(路径失效) }
        │
        ├── HKCU Run 键        （保留，备选）
        ├── 启动文件夹 lnk     （保留，备选）
        └── 计划任务            （新增，默认首选）
```

三条后端**互斥**：启用任一即清理其余。聚合层负责探测有效性、识别僵尸条目、按优先级选路。

---

## 4. 实施路线图

### P0　修 Bug，不改架构（建议先做）

预计改动集中在 `src/wakeguard.c`，约 +80 行，无新增依赖。

** P0-1　托盘图标自愈**

- 全局注册自定义消息：`g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated")`
- `WndProc` 末尾添加分支：默认兜底前判断 `msg == g_msgTaskbar`，命中则调用 `Shell_NotifyIconW(NIM_DELETE, ...)` 后重新 `NIM_ADD`
- `TrayAdd()` 内检查 `Shell_NotifyIconW` 返回值，失败时挂一次性定时器（`SetTimer` 2s）重试，最多 3 次并记日志
- 日志：每次重建写入 `Log("INFO", "tray icon re-added")`

**P0-2　自启状态三态化**

改造 `AutoStartOn()` / `AutoStartFolderOn()` 返回值为枚举而非 int：

```c
typedef enum { AS_OFF = 0, AS_ON, AS_STALE } AutoState;
```

- `AS_ON`：条目存在且解析出的目标 `GetFileAttributesW() != INVALID_FILE_ATTRIBUTES`
- `AS_STALE`：条目存在但目标文件不存在
- `AS_OFF`：条目不存在

菜单勾选仅对 `AS_ON` 生效；`AS_STALE` 时勾选标记改为灰色提示，并在诊断信息（现有 ID_DIAGNOSE 逻辑）中显式输出"路径失效"与实际指向。

Run 键的解析需取出引号内的路径；快捷方式的解析可用 `IShellLinkW::GetPath` 反向读取，或直接比对预期 `.lnk` 目标。

**P0-3　补全快捷方式属性**

在 `CreateShortcut()` 中增加：

```c
psl->lpVtbl->SetWorkingDirectory(psl, dir);
psl->lpVtbl->SetIconLocation(psl, target, 0);
```

**P0-4　辅助脚本去硬编码**

- `start_wakeguard.vbs`：改用 `WScript.ScriptFullName` 推导同级目录
- `update.bat`：待替换文件名改为命令行参数 `%~1`，缺省时自动查找 `bin\*_v*.exe`

---

### P1　新增计划任务后端

预计 +250~300 行，新增 `-lole32 -loleaut32 -luuid`（`-luuid` 已在 README 编译命令中）。

**技术选型：COM Task Scheduler 2.0 + XML 注册**

推荐用 `ITaskFolder::RegisterTask()`，传入任务 XML 字符串一步注册。相比逐个调用 `ITaskDefinition` / `ITriggerCollection` / `IActionCollection` 接口，只需声明 `ITaskService` 与 `ITaskFolder` 两个接口，在纯 C 手写 vtable 调用的场景下代码量明显更少。

**MinGW 兼容性要点**（重要）

MinGW 发行版的 `taskschd.h` / `libtaskschd.a` 支持情况不一。为避免编译期依赖，**不引用 SDK 头文件中的 GUID 符号**，改为运行时从字符串生成：

```c
CLSID clsidTaskSvc; IID iidSvc, iidFolder;
IIDFromString(L"{0F87369F-A4E5-4CFC-BD3E-73E6154572DD}", &clsidTaskSvc); /* CLSID_TaskScheduler */
IIDFromString(L"{2FABA4C7-4DA9-4013-9697-20CC3FD40F85}", &iidSvc);        /* IID_ITaskService */
IIDFromString(L"{8CFAC062-A080-4C15-9A88-AA7C2AF80DFC}", &iidFolder);     /* IID_ITaskFolder */
```

> 实施前需用 OleView 或注册表 `HKCR\TypeLib` 核对上述三个 GUID 字面量，确认无误再落地。
>
> 接口 vtable 布局（`ITaskService`、`ITaskFolder`）按 COM ABI 手写在本地 `typedef` 中，与现有 `IShellLinkW` 的做法保持一致。

**任务 XML 模板**

```xml
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Author>WakeGuard</Author>
    <Description>缓解 KB5121003 误报 AC 断开导致的随机息屏</Description>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <StartBoundary>2026-01-01T00:00:00</StartBoundary>
      <Enabled>true</Enabled>
      <Delay>PT30S</Delay>
      <UserId>当前用户 SID 或 DOMAIN\User</UserId>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>LeastPrivilege</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <StartWhenAvailable>true</StartWhenAvailable>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <RestartOnFailure>
      <Interval>PT1M</Interval>
      <Count>3</Count>
    </RestartOnFailure>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>"exe 绝对路径"</Command>
      <WorkingDirectory>exe 所在目录</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
```

关键项说明：

| 元素 | 取值理由 |
|---|---|
| `LogonType=InteractiveToken` | 必须在**用户会话**内运行，否则 Session 0 无托盘 |
| `RunLevel=LeastPrivilege` | 不需要提权，避免 UAC 弹窗 |
| `Delay=PT30S` | 错开 Explorer 托盘初始化，配合 P0-1 双重保险 |
| `DisallowStartIfOnBatteries=false` | 本项目正是笔记本电池场景，绝不能因为用电池就不启动 |
| `StopIfGoingOnBatteries=false` | 同上 |
| `MultipleInstancesPolicy=IgnoreNew` | 与现有 `CreateMutexW` 单实例保护一致，重复触发时静默忽略 |
| `ExecutionTimeLimit=PT0S` | 不限时长，常驻进程不被超时杀掉 |
| `StartWhenAvailable=true` | 错过触发时机（如休眠中）后补执行 |

**接口职责划分**

```c
static int SetAutoStartTask(int on);   /* 注册 / 删除 */
static AutoState AutoStartTaskState(void);  /* 存在且有效？ */
```

- 注册：`CoInitializeEx` → `CoCreateInstance(CLSID_TaskScheduler, IID_ITaskService)` → `Connect` → `GetFolder(L"\\")` → `RegisterTask(L"WakeGuard", xml, TASK_CREATE_OR_UPDATE, ...)`
- 删除：`ITaskFolder::DeleteTask(L"WakeGuard", 0)`
- 探测：`ITaskFolder::GetTask()` 返回 `HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)` 即视为未注册

**降级方案**

COM 初始化失败或 `taskschd.dll` 不可用时，回退到 `schtasks.exe /create /xml`（`CreateProcess` + `CREATE_NO_WINDOW`，避免闪黑窗），并把降级事实写入日志。

**命令行与菜单**

- `--autostart` 参数扩展：`on|off|folder|folderoff|task|taskoff|status|auto`
- 托盘菜单新增"开机自启（计划任务）"项，`auto` 模式下由聚合层按 `计划任务 → 启动文件夹 → Run 键` 的优先级自动选一条能成功的

---

### P2　统一管理

1. **互斥性**：启用任一后端时自动清理其余两条，保持"任一时刻只有一条生效"
2. **路径自愈**：进程启动时若发现已启用条目的目标路径 ≠ 当前 `GetModuleFileNameW()` 结果，静默重写为新路径并记日志（仅在目标文件确实缺失时触发，避免每次重写）
3. **菜单重构**：把三个自启开关收拢为一个"开机自启"子菜单，勾选状态由聚合层统一给出
4. **诊断信息升级**：现有 ID_DIAGNOSE 输出追加三后端的三态、首选级别、上次计划任务运行结果（可从 `Microsoft-Windows-TaskScheduler/Operational` 事件日志读取）

---

## 5. 验证矩阵

| # | 用例 | 环境 | 期望结果 |
|---|---|---|---|
| V1 | 冷启动（关机后开机） | Win11 26200 | 三后端各自均能拉起进程，托盘图标可见 |
| V2 | 注销后重新登录 | 同上 | **必须通过**——快速启动（Fast Startup）下"重启"≠完整引导，注销登录才是 logon trigger 的真实触发路径 |
| V3 | Explorer 崩溃重启（taskkill explorer.exe） | 同上 | P0-1 生效，图标自动重建 |
| V4 | 移动整个项目目录 | 同上 | 原后端标记 `AS_STALE`，提示或自愈 |
| V5 | 三后端两两同时启用 | 同上 | 只有一份进程、一份托盘图标 |
| V6 | 有第三方安全软件拦截 Run 键 | 模拟 | 计划任务路径不受影响 |
| V7 | 安装 `update.bat` 升级 | 同上 | 升级后自启条目仍指向新 exe |
| V8 | 电池供电下登录 | 笔记本 | 计划任务不被 `DisallowStartIfOnBatteries` 挡住 |

> 关于快速启动的补充：Windows 默认开启 Fast Startup 时，"关机→开机"走的是休眠恢复路径，**boot trigger 不触发，但 logon trigger 会**。这也是选 logon 而非 boot trigger 的原因之一。

---

## 6. 风险与取舍

| 风险 | 影响 | 应对 |
|---|---|---|
| `taskschd.h` / GUID 在 MinGW 下不匹配 | 编译或运行时失败 | 用 `IIDFromString` 绕开符号依赖；实施前先核对 GUID |
| Task Scheduler 服务被禁用 | 计划任务不执行 | 聚合层探测服务状态，失败自动降级到启动文件夹 |
| 新增 ~300 行破坏 87KB / 2.5MB 的体积优势 | 轻微 | 纯 COM 手写调用，不引入静态库；体积增量预计 < 10KB |
| 计划任务 XML 版本 1.4 在旧系统不识别 | Win7 等 | `RegisterTask` 失败时降级；本项目基线为 Win10+ |
| 三条路径互斥可能让用户原有的双启用配置失效 | 使用习惯变化 | 迁移时保留用户已启用的一条，不强制重置 |

---

## 7. 建议执行顺序

1. **P0 全部**（修 bug，无风险，建议合并为一个 commit：`fix: 托盘图标自愈与自启状态校验`）
2. **P1 中「任务 XML + RegisterTask 注册」的最小闭环**，命令行先跑通
3. **P1 剩余**（探测、降级、托盘菜单接入）
4. **P2**（互斥、自愈、菜单重构）

每步完成后重跑验证矩阵 V1–V8。
