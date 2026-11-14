# C++ Watchdog 重写设计规格

> 状态：Approved for implementation（2026-11-14）
> 日期：2026-11-14
> 目标平台：Windows 10/11 x64；WPS Office 12.1 + Microsoft Edge
> 适用范围：htmlplppt 模式 B/B2 的 watcher、黑场、聚焦、清理与配置体系
> 参考实现：`scripts/watch-live-content.vbs`、`scripts/watch-slides.vbs`、`scripts/black-fade.ps1`、`scripts/focus-agent.ps1`、`scripts/focus-lib.ps1`、`scripts/bring-wps.ps1`、`scripts/bring-edge.ps1`、`scripts/stop-live.vbs`、`scripts/stop-watch.vbs`
> 相关文档：`docs/01-architecture.md`、`references/live-content-trigger.md`、`references/live-browser-mode.md`

---

## 1. 背景与目标

现有 Windows 侧车系统由 VBScript 与 PowerShell 组合实现：

- `watch-live-content.vbs` 轮询 WPS COM，维护目标页状态，通过标志文件协调黑场、聚焦 agent 与 Edge 启动。
- `black-fade.ps1` 用 WinForms 实现全屏黑场。
- `focus-agent.ps1` 常驻轮询 Edge 窗口标题，负责 ready/return/exit 信号。
- `focus-lib.ps1`、`bring-wps.ps1`、`bring-edge.ps1` 负责窗口查找与 Windows 前台锁解锁。
- `stop-live.vbs`、`stop-watch.vbs` 负责应急清理。

现有实现已经验证可用，但存在以下结构性问题：

1. 黑场依赖固定 350ms 睡眠与 3 秒保持上限，Edge 冷启动较慢时会露出 WPS 编辑器或桌面。
2. 退出检测完全依赖 `focus-agent`；agent 缺失、崩溃或睡眠恢复后心跳失效时，watcher 可能卡在 `state=1` 最长 600 秒。
3. 返回阶段 focus-agent 与 bring-wps 会争夺前台焦点。
4. `Cleanup` 与停止脚本按脚本名全局杀进程，可能误杀另一个演示目录的 helper。
5. 旧 watcher 使用文件轮询、墙钟 `Timer` 与固定 100ms 节奏，调度上限低。
6. `watch-slides.vbs` 仍把任何 COM 瞬时错误当成致命错误，未继承 B2 watcher 的重试与清理能力。

本项目的目标是用 C++ 重写为单 EXE 的原生 watchdog，在完全继承现有用户可见行为的前提下，获得：

- 跨进程自愈与黑匣子诊断能力。
- 事件驱动 + 自适应高频调度。
- 精确的进程/窗口归属与幂等清理。
- 可自由配置的 PPT 页到 HTML 内容映射，以及原生配置 UI。
- 明确的失败分类、回退路径与风险预留点。

### 1.1 非目标

- 不修改 WPS/PowerPoint 运行时，不使用宏、ActiveX、代码注入或联网插件。
- 不要求管理员权限，不注册 Windows 服务，不写系统级策略。
- 不实现登录自启动、双 worker 热备、远端遥测。
- PowerPoint 只保留宿主接口与实验开关，不做与 WPS 等价的完整验收。
- 不删除现有 VBS/PowerShell 资产；它们保留为回退路径与 A/B 对照。

---

## 2. 已确认决策

| 决策项 | 选择 |
|---|---|
| 进程模型 | 单 EXE 双角色：轻量 supervisor + 业务 worker；同一二进制，`--supervisor` / `--worker` 内部切换 |
| 兼容边界 | 原生内核 + 兼容层；保留标志文件、`TEF-LIVE-RETURN` 标题契约、CONFIG 语义、`stop-live.vbs` 入口 |
| 构建形态 | CMake + C++20；MSVC 2022 主构建，MinGW-w64 交叉编译作为 WSL 内构建与兼容性验证 |
| 调度形态 | COM 事件 sink + 自适应高频轮询 + 线程分级；aggressive / adaptive / balanced / safe 档 |
| 韧性层级 | 进程级自愈 + 黑匣子 + journal + 崩溃转储；不做系统级自启动、不做双 worker 热备 |
| 配置来源 | 单 EXE + 可选 `watcher.json` + CLI；配置可热加载；v1 同时提供原生配置 UI |
| 宿主范围 | WPS 完整实现；PowerPoint 预留 `PresentationHost` 接口与实验开关 |
| 部署安全 | 未签名 EXE 策略不确定；预留自动/手动回退到现有 VBS + PowerShell 流程 |
| 失败策略 | 分类自愈：瞬时失败重试，切换失败清除 done 允许重试，结构性失败进入 SafeMode，可选 legacy 回退 |
| 配置入口（v1） | JSON 手写、`--init-config` 生成器、CLI `--map`、原生 `--config-ui` |
| 配置入口（预留） | PPT 备注标签 `@html:`、文件名约定运行时自动发现、WebView2 内嵌预览 |

---

## 3. 现有行为契约

以下行为必须保留。状态机、配置层和兼容层都以此为准。

### 3.1 启动

1. 校验配置数组等长、deck 与 HTML 存在、Edge 可定位。
2. 获取单实例锁。
3. 预热黑场与聚焦子系统。
4. 启动 WPS，打开 deck（只读），从第 1 页开始放映并聚焦。
5. 记录当前页为 `prevSlide`，设置约 1 秒启动忽略窗口。
6. `/test` 模式在启动后跳到第一个目标页。

### 3.2 触发

- 仅当“离开目标页且向前翻页”时触发：
  `pos > prevSlide && prevSlide ∈ targetSlides && !done[prevSlide]`
- 触发时先把当前目标页标记 done，再启动 Edge。
- 同一场放映每个目标页只触发一次。
- 放映结束（`SlideShowWindows.Count == 0`）时重置 done，`prevSlide = -1`，等待用户重新开始放映。

### 3.3 切换与返回

- 触发后渐黑，随后启动 Edge、退出 WPS 放映。
- 返回请求来自 HTML：页面标题改为 `TEF-LIVE-RETURN`，页面内渐黑，2 秒后 `window.close()`。
- watcher 收到返回信号后渐黑、恢复 WPS 放映、聚焦、跳回原目标页。
- 返回后 `ignoreUntil` 约 2 秒，避免把恢复过程误判为翻页。
- Edge 被 Alt+F4 等无信号方式关闭时，走“关闭后渐黑恢复”的兜底路径。

### 3.4 HTML 契约

- 页面自包含或与资源目录一起部署。
- 空格推进步骤，同时判断 `e.code === "Space"` 与 `e.key === " "`。
- 最后一步空格或 Esc 进入返回流程，不直接 `window.close()`。
- 返回流程设置 `document.title = 'TEF-LIVE-RETURN'`，页面内渐黑，至少延迟 2 秒关闭。
- 页面启动时调用 `window.focus()`，避免首次按键丢失。

### 3.5 Edge 契约

- 使用 `--app=file:///...`、`--start-fullscreen`、独立 `--user-data-dir`。
- 保留现有同步/首启抑制参数组：
  `--no-first-run --no-default-browser-check --disable-sync`
  `--disable-features=msImplicitSignin,msEdgeSync,msFirstRunExperience,msEdgeFirstRunExperience,EdgeSync`
  `--disable-background-networking --disable-component-update --disable-extensions`
  `--disable-default-apps --no-service-autorun --disable-client-side-phishing-detection`
  `--disable-domain-reliability --no-pings --disable-breakpad`
- 每个目标页使用独立 Edge profile，避免窗口归属歧义。

### 3.6 COM 容错

- WPS 在切换/退出/恢复时可能出现瞬时 COM 错误（实测 `err=462`）。
- 连续错误达到约 2 秒窗口才判定 WPS 不可用；成功一次即重置错误窗口。
- 判定不可用后执行清理并退出。
- 原有“20 次 × 100ms”的语义以时间窗口方式保留，阈值可配置。

### 3.7 超时与测试

- Edge 最长存活时间默认 600 秒。
- `/test` 或 `--test` 跳到第一个目标页，用于验收。
- 正式演示不启用测试跳页。

### 3.8 配置语义

现有六个数组必须能 1:1 迁移：

- `deckName`
- `targetSlides`（1-based）
- `srcNames`
- `targetHtmls`（ASCII 暂存名）
- `targetProfiles`
- `targetLabels`

所有数组等长；页号在 deck 范围内；Edge profile 唯一。

### 3.9 兼容面

- 继续写同名标志文件：`watcher.lock`、`cover-fade.flag`、`cover-ready.flag`、`cover-done.flag`、`cover-stop.flag`、`cover-hide.flag`、`agent-arm.flag`、`agent-stop.flag`、`return-request.flag`、`edge-closed.flag`。
- 继续写 `watch-log.txt`、`bring-log.txt`、`cover-log.txt`，保留关键日志词：
  `cover started`、`agent started`、`trigger slide N -> Label`、`return requested by page`、`resumed at slide N`、`edge closed after return; cover fade out`、`COM busy`、`cleanup start`、`cleanup done`。
- `stop-live.vbs` 仍可作为停止入口；内部改为优先调用 `htmlppt-watchdog.exe --stop`，找不到 EXE 时回退原有 taskkill 行为。
- 同一配置目录单实例；不同 demo 目录可并行。

---

## 4. 架构

### 4.1 单 EXE 双角色

`htmlppt-watchdog.exe` 默认以 supervisor 身份启动。内部角色：

- `--supervisor`：默认入口。创建 Job Object、启动 worker、收心跳、重启、转储、进入 SafeMode。
- `--worker`：业务进程。承载 COM、状态机、黑场、聚焦、Edge 作业、清理、兼容层。
- `--stop`：向运行中的实例发送停止信号并等待清理。
- `--status`：读取共享内存/journal，输出状态。
- `--self-test`：COM、Edge、overlay、focus、定时器、Edge profile 匹配自检。
- `--config-ui`：原生配置器。
- `--dump-helper`：独立崩溃转储辅助模式。

### 4.2 Supervisor

supervisor 保持极简，只做六件事：

1. 创建命名互斥体与 Job Object。
2. 以挂起方式创建 worker，加入 Job，再恢复运行。
3. 通过命名管道接收 200ms 心跳与状态快照。
4. 2 秒无心跳时通过 `--dump-helper` 保存 mini dump，终止 worker，按退避重启。
5. 重启预算耗尽后进入 SafeMode，隐藏黑场、尽力恢复 WPS、输出诊断。
6. supervisor 自身退出时关闭 Job，确保 worker 与自有 Edge 不残留。worker 为每次触发创建独立 Edge Job，并把 Job 句柄复制给 supervisor；worker 崩溃时 supervisor 可终止该 Job，避免残留 Edge。

重启预算：10 分钟内最多 5 次；退避 200ms、500ms、1s、2s、5s，带抖动。

supervisor 不解析业务配置、不调用 COM、不创建窗口。其代码路径与数据结构保持最小，避免自身成为故障源。

### 4.3 Worker 线程

| 线程 | 职责 | 约束 |
|---|---|---|
| COM/状态线程 | STA；独占 `Kwpp.Application` COM 调用；状态机主循环；事件队列消费；STA 消息泵 | 所有 COM 对象只在此线程创建/释放；`CoEnableCallCancellation` 可用 |
| UI/黑场线程 | layered topmost no-activate click-through 全屏窗口；渐黑/保持/渐透明；`coverHold`/`coverDone` 信号 | 独立消息泵；不阻塞等待状态线程 |
| 聚焦线程 | Edge/WPS 窗口枚举、Edge profile 精确匹配、前台解锁、焦点校验、标题变化、Edge 关闭监测 | 不持有状态机锁；结果入队 |
| IPC/健康线程 | 向 supervisor 发心跳与状态快照；接收 stop/restart；写 journal 缓冲 | 非阻塞 overlapped I/O |
| 清理线程 | 幂等清理 Edge Job、overlay、标志、锁、COM 引用 | 可重复执行；不依赖其他线程存活 |

线程间通过有界 MPSC 队列通信。`coverHold`、`edgeClosed`、`returnRequested`、`stop` 为不可丢弃事件；翻页事件可合并。

### 4.4 分层

- `core`：状态机、配置模型、journal、黑匣子、时钟抽象、重试/熔断、调度策略、诊断。尽量不依赖 Win32，可在 Linux 上单元测试。
- `platform/win32`：COM host、事件 sink、窗口枚举、前台解锁、overlay、Job/进程、IPC、崩溃转储、电源与定时器。
- `compat`：标志文件镜像、legacy CONFIG 映射、legacy 脚本回退、`--stop` 兼容入口。
- `supervisor`：监控与恢复。
- `cli`：参数解析、模式分发、诊断命令。

### 4.5 数据流

```text
WPS COM 事件/轮询 ─┐
WinEvent 回调 ─────┼─> 事件队列 ─> 状态机 ─┬─> overlay 指令
Edge 进程句柄 ─────┤                        ├─> 聚焦指令
定时器/超时 ───────┘                        ├─> Edge 作业控制
                                             ├─> 兼容标志镜像
                                             └─> journal/黑匣子
```

状态机是唯一状态所有者。其他线程只上报事实，不直接修改状态。

### 4.6 IPC 与状态共享

- supervisor 创建共享内存区（默认 8 MB），包含状态快照、心跳槽、黑匣子环形缓冲。
- worker 通过命名管道发送变长日志/事件帧；共享内存用于高频心跳与崩溃快照。
- 帧格式：魔数、版本、序号、时间戳、类型、长度、CRC32。
- supervisor 只读共享内存；worker 写。序号单调递增，supervisor 检测停滞。

### 4.7 单实例与多目录

- 命名互斥体名称由“配置目录规范路径指纹 + 配置哈希”生成。
- 不同 demo 目录可并行运行，互不杀进程。
- `watcher.lock` 继续存在，内容为包含 `run_id`、`pid`、`config_hash` 的 JSON 行；旧工具只检查存在性与修改时间，保持兼容。
- 旧锁无 `run_id` 时视为 legacy；超过 15 秒未更新可回收。

### 4.8 线程规则

- 禁止 `TerminateThread`；使用 stop token + 可等待事件协作退出。
- 禁止无限 `join`/`WaitForSingleObject`；所有等待带超时。
- COM 对象遵循 STA 归属；事件回调只入队，不做 UI 或阻塞操作。
- 句柄、COM 指针、GDI/USER 对象使用 RAII。
- 调试构建启用线程断言：COM 调用线程、overlay 线程、状态机线程的归属检查。

---

## 5. 状态机

### 5.1 状态与迁移

| 状态 | 进入动作 | 关键事件 | 迁移 |
|---|---|---|---|
| Boot | 解析配置、单实例、能力探测、journal 对账 | 通过 / 失败 | Warming / Faulted |
| Warming | 启动 overlay、聚焦子系统、COM；打开 deck；开始放映；聚焦；测试跳页 | showStarted | Monitoring / Faulted |
| Monitoring | 维护 `prevSlide`、`ignoreUntil`、`doneFlags`；按距离调整轮询 | 离开目标页、放映结束、stop | CoverIn / Monitoring / Stopping |
| CoverIn | 请求渐黑；等待 `coverHold` | coverHold / timeout | EdgeStarting / Faulted |
| EdgeStarting | 标记 done；arm 聚焦；启动 Edge；等待 `edgeReady` | edgeReady / timeout | ShowExit / Faulted |
| ShowExit | 退出 WPS 放映并校验 | exited / retry exhausted | Live / Faulted |
| Live | 监测返回标题、Edge 关闭、600s 超时、WPS 故障 | returnRequested / edgeClosed / timeout / stop | Returning / Monitoring / Faulted |
| Returning | 渐黑；恢复 WPS；聚焦；跳回目标页 | showResumed / timeout | ReturnCoverOut / Faulted |
| ReturnCoverOut | 写 cover-ready；等待淡出完成 | coverDone | Monitoring |
| Stopping | 幂等清理 | done | Exited |
| Faulted | 隐藏黑场、尽力恢复 WPS/前台、记录诊断、可选 legacy 回退 | continue / abort | SafeMode / Exited |
| DegradedNoSupervisor | supervisor 失联，继续完成当前流程或清理 | continue / cleanup | Monitoring / Stopping / Exited |

### 5.2 事件

- `slideChanged(oldPos, newPos, monotonicTs)`
- `showEnded(monotonicTs)`
- `showStarted(monotonicTs)`
- `coverHold(monotonicTs)`：黑场透明度达到 1.0。
- `coverDone(monotonicTs)`：渐透明完成。
- `edgeReady(pid, hwnd, monotonicTs)`：Edge 窗口找到并成为前台。
- `edgeClosed(pid, monotonicTs)`：Edge 主进程退出或窗口销毁。
- `returnRequested(monotonicTs)`：检测到 `TEF-LIVE-RETURN`。
- `timeout(kind, monotonicTs)`
- `stopRequested(source)`
- `powerResume(monotonicTs)`
- `configReloaded(generation)`
- `workerFault(threadId, reason)`

事件携带单调时间戳；墙钟时间只用于日志关联。

### 5.3 守卫与不变量

- `ignoreUntil`：启动与返回后忽略位置变化。
- `prevSlide`：上一次观测到的放映页；恢复后置 -1。
- `doneFlags[]`：本场放映已触发目标页。
- `currentTarget`：当前正在进行的触发目标。
- `returnPrepared`：返回请求已处理，避免 Edge 关闭后重复恢复。
- `edgeDeadline`：Live 状态的最长等待。
- `triggerGeneration`：每次触发递增，用于丢弃过期事件。
- `comErrorWindow`：时间窗口内的连续 COM 错误；成功即清零。

不变量：

1. 同一 `triggerGeneration` 只允许一次 Edge 启动。
2. `done` 在任何 Edge 启动前写入；失败策略可选择回滚。
3. `returnPrepared` 为真时，Edge 关闭只触发 cover-ready，不再次 ResumeShow。
4. `cover-ready` 只能在 WPS 恢复成功后写。
5. 任何故障路径都不得让 overlay 永久保持可见。

### 5.4 失败策略

按失败类别处理：

- **瞬时失败**：COM 462、焦点竞争、Edge 窗口短暂重建、标题短暂不可读。有限重试 + 抖动；成功后继续。
- **切换失败**：
  - 黑场未在超时内达到 `coverHold`；
  - Edge 在超时内未 `edgeReady`；
  - 退出 WPS 放映重试耗尽。
  处理：隐藏黑场；终止本次自有 Edge Job；若 WPS 仍在放映则继续，否则恢复放映并跳回目标页；`onFailure=self-heal` 时清除当前页 done 以允许重试；连续失败达到 `maxConsecutiveTriggerFailures`（默认 3）后进入 SafeMode。`onFailure=strict` 时保留 done，行为与旧脚本一致。
- **返回失败**：WPS 恢复重试耗尽。处理：隐藏黑场；显示非阻塞诊断；保留 Edge 让用户手动关闭；进入 Faulted，后续 `edgeClosed` 只记录，不再自动 ResumeShow，避免循环；用户可手动重试或停止。
- **结构性失败**：配置缺失、HTML 不存在、Edge 不可用、WPS COM 不可用。处理：立即清理，进入 SafeMode，可选 legacy 回退。
- **停止/超时**：隐藏黑场；终止自有 Edge Job；停止辅助线程；释放锁。
- **supervisor 失联**：默认继续完成当前流程（`--on-supervisor-loss=continue`），写 critical 日志；可配置为 cleanup。

### 5.5 时钟与超时

- 所有间隔使用 `QueryPerformanceCounter` / `GetTickCount64`。
- 墙钟仅用于日志与 journal 关联。
- 默认超时：
  - 启动后忽略窗口：1s
  - 返回后忽略窗口：2s
  - 黑场渐黑：300ms
  - 黑场最短保持：1000ms
  - 黑场安全上限：10000ms
  - Edge ready 超时：20s
  - Edge 最长存活：600s
  - 聚焦单次超时：6s
  - COM 错误窗口：2s
  - supervisor 心跳：200ms / 2s
  - worker 重启预算：10 分钟内 5 次

---

## 6. 韧性与风险

### 6.1 双层看门狗

- supervisor 监控 worker 进程；worker 内部线程 watchdog 监控各线程心跳。
- 每线程注册心跳槽：原子序号、状态、最后心跳 QPC。
- 100ms 检查一次；超过 1s 未心跳则记录并尝试恢复。
- COM 线程卡死：优先 `CoEnableCallCancellation` / `CoCancelCall`；不可取消则通知 supervisor 进程级重启。
- 线程无法协作退出时，worker 通过 IPC 请求 supervisor 重启，禁止调用 `TerminateThread`。

### 6.2 Journal 与启动对账

- 路径：`%LOCALAPPDATA%\htmlppt-watchdog\<config-hash>\journal.jsonl`，可用 `--state-dir` 覆盖。
- 记录：`run_id`、序号、单调时间、状态、事件、目标页、done 位、Edge PID/edgeProfile、WPS 放映状态、cover 阶段、HRESULT、线程心跳摘要。
- 关键迁移立即 flush；普通事件批量 flush。
- 启动对账：
  1. 读取最后状态。
  2. 检查遗留 Edge PID/edgeProfile/Job 是否存在。
  3. 检查 overlay 窗口是否属于本实例。
  4. 按 `run_id` 判断标志文件归属；可接管则接管，否则幂等清理。
  5. 若上次处于 Live/Returning，优先尝试恢复 WPS 到目标页；失败则清理到 Monitoring 或 SafeMode。
- 所有清理动作必须可重复执行。

### 6.3 黑匣子

- supervisor 创建共享内存环形缓冲（默认 8 MB，固定长度记录，原子序号）。
- 记录：事件、状态、QPC 时间、耗时、HRESULT、窗口句柄、PID、前台策略结果、线程心跳、句柄/GDI/内存计数。
- worker 各线程无锁写入；supervisor 在崩溃/挂起时直接快照。
- 正常运行周期落 JSONL；`--diagnostics` 打包 journal、黑匣子、日志、配置快照、系统信息。
- 环形缓冲满时覆盖最旧记录，绝不阻塞关键线程。

### 6.4 崩溃转储与重启

- supervisor 检测到 worker 异常退出或挂起时，启动 `--dump-helper <pid> <path>` 子进程执行 `MiniDumpWriteDump`，避免阻塞监控循环。
- dump 路径：`%LOCALAPPDATA%\htmlppt-watchdog\<config-hash>\crash\`。
- 重启前执行 Job 清理与 journal 对账。
- 重启预算耗尽进入 SafeMode；可选 legacy 回退。
- 用户可见的 SafeMode 行为：隐藏黑场、恢复或保留 WPS、显示非阻塞的 TaskDialog/通知、写 critical 日志；不得静默退出。

### 6.5 降级与回退

- `--legacy-fallback=auto|force|off`：
  - `auto`：原生自检失败、连续崩溃达到阈值、或关键能力缺失时，启动现有 VBS + PowerShell 流程。
  - `force`：直接使用 legacy 流程。
  - `off`：只使用原生流程。
- EXE 被安全软件拦截时无法自动回退，但旧脚本仍在 demo 目录，可手动运行。
- `--on-supervisor-loss=continue|cleanup`：
  - `continue`：完成当前切换/返回，写 critical 日志。
  - `cleanup`：执行幂等清理后退出。

### 6.6 故障注入

仅诊断模式可用：

- `--fault=com-hang`
- `--fault=com-462`
- `--fault=edge-slow`
- `--fault=cover-hang`
- `--fault=focus-deny`
- `--fault=supervisor-loss`
- `--fault=sleep-resume`
- `--fault=multi-monitor`

每个故障注入必须有对应的自动化测试或验收步骤。

### 6.7 风险登记表

| 编号 | 风险 | 缓解设计 | 预留钩子 |
|---|---|---|---|
| R1 | WPS COM 事件不可用/版本差异 | 能力探测；事件 sink 与轮询双路径 | `--poll-only`、`PresentationHost` |
| R2 | COM 462 等瞬时错误 | 时间窗口重试、抖动、熔断 | 重试策略配置、`--fault=com-462` |
| R3 | Edge 冷启动超过黑场上限 | `coverHold` 握手；可配置 10s 上限；`edgeReady` 后再退放映 | `--cover-max-hold`、失败分类 |
| R4 | Windows 前台锁策略变化 | 多策略链 + 结果校验 + 成功率指标 | `--focus-strategy` |
| R5 | overlay 被 topmost 抢占 | 监听 `EVENT_SYSTEM_FOREGROUND` 重新置顶；阶段切换重新断言 | `--topmost-guard=on/off` |
| R6 | Edge profile 锁/残留进程 | 精确 run-id/edgeProfile/Job 归属；陈旧 profile 检测 | 接管/清理策略、`--fresh-profile` |
| R7 | 未签名 EXE / AV / SmartScreen | manifest、版本信息、SHA256、无混淆、可选签名 | `--self-test`、legacy 回退 |
| R8 | 睡眠/唤醒、时钟跳变 | 单调时钟、`WM_POWERBROADCAST`、定时器重臂 | `--on-resume=resume\|restart\|cleanup` |
| R9 | 多显示器/DPI 变化 | PerMonitorV2、虚拟屏幕、`WM_DISPLAYCHANGE` 重算 | 显示器拓扑快照 |
| R10 | 多实例/多目录 | 配置目录指纹 + 配置哈希互斥体 | run-id 贯穿日志/标志/进程 |
| R11 | WPS 崩溃或被强杀 | HRESULT 分类、清理、supervisor 重启 | 崩溃恢复策略 |
| R12 | 用户 Esc/Alt+F4 手动干预 | 检测放映结束/窗口消失；黑场 fail-safe 隐藏 | 手动干预事件 |
| R13 | CPU/功耗/散热 | 四档调度；CPU 预算与退避 | `--scheduling`、EcoQoS 控制 |
| R14 | COM 重入/STA 违规 | COM 单线程；事件回调入队 | 线程断言、调试构建检查 |
| R15 | 配置漂移/非法 JSON | schema 校验、哈希、安全点热加载 | `--print-config`、`--validate` |
| R16 | 日志/磁盘写满 | 有界日志、滚动、回退到 `%TEMP%` | 磁盘阈值告警 |
| R17 | PowerPoint 行为差异 | Host adapter 最小实现 + 实验开关 | `--host=powerpoint` |
| R18 | 配置文件指向目录外 HTML | 默认拒绝，显式开关 | `--allow-outside-folder` |
| R19 | 线程优先级导致系统饥饿 | 不使用 REALTIME；CPU 预算；自动降档 | `--scheduling`、`--cpu-budget` |
| R20 | 黑匣子/诊断目录权限不足 | 回退到 demo 目录或 `%TEMP%`；告警 | `--state-dir` |

### 6.8 安全与权限

- manifest 使用 `asInvoker`；不要求管理员；不注册服务；不联网。
- 只终止自己 Job/edgeProfile 归属的 Edge 进程；不按脚本名全局杀进程。
- 合成 Alt 仅用于解除前台锁，不向演示内容注入按键。
- 默认拒绝配置指向 demo 目录之外的 HTML；显式 `--allow-outside-folder` 才放行。
- 日志与诊断包不包含敏感内容，只包含路径、版本、时间线、句柄与 PID。
- 不混淆、不加壳；提供 SHA256 与版本资源，降低 AV 误报；签名作为可选发布步骤。

---

## 7. 调度

### 7.1 事件优先

| 事件源 | 用途 | 延迟 | 成本 |
|---|---|---|---|
| COM 事件 sink（WPS `SlideShowNextSlide`/`SlideShowEnd`/`SlideShowBegin`，若可用） | 翻页、放映开始/结束 | 事件级 | 低，依赖 WPS |
| `SetWinEventHook`（`EVENT_OBJECT_NAMECHANGE`、`EVENT_OBJECT_DESTROY`、`EVENT_SYSTEM_FOREGROUND`） | 返回标题、Edge 窗口销毁、前台变化 | 事件级 | 低 |
| 进程句柄等待 | Edge 主进程退出 | 事件级 | 极低 |
| 高精度定时器 + WPS 位置轮询 | 兜底与目标接近检测 | 1-20ms | 可控 |

- COM 事件不可用时自动降级为纯轮询。
- 事件回调只做入队、序号、时间戳；状态机单线程消费。
- Edge 关闭同时监听进程句柄、Job 进程数、`EVENT_OBJECT_DESTROY`。

### 7.2 自适应轮询

| 状态/距离 | aggressive | adaptive 默认 | balanced | safe |
|---|---|---|---|---|
| Monitoring，远离目标页 | 5ms | 10ms | 10ms | 20ms |
| Monitoring，距目标页 ≤2 页 | 1ms | 2ms | 2ms | 10ms |
| 位于目标页 | 1ms | 1ms | 2ms | 5ms |
| CoverIn / EdgeStarting / ShowExit | 1ms | 1ms | 1-2ms | 5ms |
| Live | 1ms | 1ms | 2ms | 10ms |
| Returning / ReturnCoverOut | 1ms | 1ms | 1-2ms | 5ms |
| Stopping / Faulted | 10ms | 10ms | 20ms | 50ms |

- 默认档为 `adaptive`：关键窗口 1ms，远离目标时退避。
- `aggressive` 强制关键状态 1ms，并允许 200µs 有界自旋。
- `safe` 用于 AV/低功耗/兼容性排查。
- 定时器使用 `CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`；不可用时回退 `timeBeginPeriod(1)` + 普通可等待定时器。

### 7.3 线程分级与电源

| 线程 | 优先级 | 电源/亲和性 |
|---|---|---|
| COM/状态线程 | `THREAD_PRIORITY_HIGHEST`（aggressive 可配 `TIME_CRITICAL`） | 关闭 EcoQoS；可选 P-core CPU Set |
| 聚焦线程 | `THREAD_PRIORITY_ABOVE_NORMAL` | 关闭 EcoQoS |
| UI/黑场线程 | `THREAD_PRIORITY_NORMAL` | 正常 |
| IPC/健康线程 | `THREAD_PRIORITY_BELOW_NORMAL` | 正常 |
| supervisor | `THREAD_PRIORITY_BELOW_NORMAL` | 正常 |

- 进程优先级 `ABOVE_NORMAL_PRIORITY_CLASS`；不使用 `REALTIME_PRIORITY_CLASS`。
- `SetProcessInformation(ProcessPowerThrottling)` / `SetThreadInformation(ThreadPowerThrottling)` 控制 EcoQoS。
- 可选 `--cpu-set=auto|none`：混合架构 CPU 上倾向 P-core；默认 `none`。
- 演示期间 `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)`；停止/故障时恢复。
- `GetSystemPowerStatus` 检测电池；电池默认降为 `balanced`，可强制。

### 7.4 时间预算与指标

不含 WPS COM 自身耗时：

- 事件入队到状态机处理：p99 < 1ms（aggressive/adaptive 关键窗口）
- 触发判定到黑场全黑：p99 < 350ms
- Edge 出现到 `edgeReady`：p99 < 10ms
- `TEF-LIVE-RETURN` 变化到返回启动：p99 < 10ms
- Edge 关闭到 `edgeClosed`：p99 < 10ms
- 黑场渐变帧间隔抖动：p99 < 5ms
- 空闲 CPU：balanced/safe < 1%；aggressive 插电 < 5%

插桩点：事件时间、状态进入/退出、cover 阶段、聚焦尝试与结果、Edge 启动/关闭、返回恢复、COM 调用耗时、线程心跳。`--benchmark` 输出 p50/p99，`--status` 输出最近时间线。

### 7.5 事件循环

- 状态线程使用 `MsgWaitForMultipleObjectsEx`，同时等待：高精度定时器、停止事件、IPC 事件、WinEvent 队列信号、Edge 进程句柄；并泵送 STA 消息。
- 输入进入有界 MPSC 队列；队列满时合并翻页事件，关键事件永不丢弃。
- 文件标志只在兼容层低频轮询/写出，不作为主调度源。
- 所有等待带超时；状态线程不执行长时间阻塞操作。

---

## 8. 配置

### 8.1 来源与优先级

```text
CLI --map / --target
  > profile 内 targets
  > 顶层 targets
  > legacy 数组
```

- `defaults` 对全部配置 profile 生效；profile 与 target 可逐字段覆盖。
- v1 不支持运行时 PPT 备注标签与文件名自动发现；它们作为 v2 预留。
- 自动映射向导属于编辑期辅助，不是运行时发现。
- 冲突默认报错；`--allow-override` 显式允许并记录胜出者。

### 8.2 顶层 schema

```json
{
  "version": 1,
  "activeProfile": "competition",
  "defaults": {
    "trigger": "leave-forward",
    "cover": { "fadeMs": 300, "holdMs": 1000, "maxHoldMs": 10000 },
    "edge": { "maxLiveMs": 600000, "prewarmProfile": false },
    "display": { "monitor": "primary" },
    "focus": { "strategy": "auto", "require": true },
    "scheduling": "adaptive",
    "onFailure": "self-heal",
    "maxConsecutiveTriggerFailures": 3
  },
  "profiles": {
    "competition": {
      "deck": "deck-live.pptx",
      "stateDir": "%LOCALAPPDATA%/htmlppt-watchdog",
      "stage": { "mode": "file", "root": "%TEMP%/tef-live-stage" },
      "targets": [
        {
          "slide": 5,
          "html": "pages/audit/index.html",
          "label": "Audit",
          "edgeProfile": "tef-live-audit",
          "stageName": "audit.html",
          "assets": { "mode": "tree", "root": "pages/audit" },
          "htmlQuery": "?lang=zh#summary",
          "cover": { "maxHoldMs": 15000 },
          "display": { "monitor": 2 },
          "enabled": true
        },
        {
          "match": { "slideTitle": "合同引擎", "mode": "exact" },
          "html": "pages/contract.html",
          "label": "Contract"
        }
      ]
    }
  }
}
```

单 profile 简写：不需要多 profile 时，可以把 `deck`、`stage`、`stateDir`、`targets` 直接放在顶层，省略 `profiles` 与 `activeProfile`。CLI `--profile NAME` 只用于从 `profiles` 中选择；单 profile 配置忽略该参数。存在多个 profile 且未指定 `activeProfile` 时，必须通过 `--profile` 选择，否则启动报错并列出可用项。

`edgeProfile` 是每个目标页的 Edge `--user-data-dir` 后缀，不要与配置 profile 混淆。`deck` 省略时自动探测配置目录中唯一的 `.pptx`；存在多个时必须显式指定。
```

### 8.3 目标页定位

- `slide`：1-based 页号。
- `match.slideTitle`：启动时通过 COM 读取标题占位符或 `Slide.Name`；支持 `exact`、`prefix`、`regex`；重名报错。
- `match.slideName`：读取 `Slide.Name`。
- 页号与标题匹配二选一；同时出现视为配置错误。
- v2 预留：备注标签 `@html:`、文件名约定、PPT 自定义属性。

### 8.4 HTML 承载模式

| 模式 | 行为 | 适用 |
|---|---|---|
| `file` | 只复制单个 HTML 到 ASCII 暂存目录；要求自包含 | 当前行为，默认 |
| `tree` | 复制 HTML 目录或 `assetRoot` 到 ASCII 暂存目录，保留相对路径 | 带 CSS/JS/图片 |
| `direct` | 不复制，`UrlCreateFromPathW` 百分号编码后以 `file:///` 启动 | 大资源、中文/UNC 路径 |

- `tree` 在启动时一次性复制，不在触发时复制。
- `tree` 支持 `include`/`exclude` glob 与最大体积限制。
- `direct` 保留为兼容性选项；若 Edge 打开空白页，回退 `file` 或 `tree`。
- 同一 HTML 可通过 query/hash 复用到多页。
- 每页独立 Edge profile；`edgeProfile` 省略时由 deck 哈希 + target id 自动生成。
- v2 预留：`sequence` 多 HTML 链（通过 `TEF-LIVE-NEXT` 扩展）。

### 8.5 每目标字段

| 字段 | 说明 |
|---|---|
| `slide` / `match` | 目标页定位 |
| `html` | HTML 路径，可带 query/hash |
| `label` | 日志标签 |
| `edgeProfile` | Edge `--user-data-dir` 后缀；默认自动生成 |
| `stageName` | ASCII 暂存文件名；默认自动生成 |
| `assets` | `mode`、`root`、`include`、`exclude`、`maxBytes` |
| `htmlQuery` | 附加 query/hash；若 `html` 已包含 query/hash，则合并，冲突报错 |
| `trigger` | `leave-forward`（默认）/ `arrive` / v2 热键/外部事件 |
| `cover` | 每页覆盖黑场时序 |
| `edge` | 每页覆盖 `maxLiveMs`、`args`、`prewarmProfile` |
| `display` | `primary` / `secondary` / 1-based 索引 |
| `focus` | 策略与是否要求聚焦 |
| `onFailure` | `self-heal`（默认）/ `strict` / `safe` / `legacy` |
| `maxConsecutiveTriggerFailures` | 每页覆盖连续失败阈值；默认继承全局值 |
| `enabled` | 是否启用 |
| `tags` | 自由元数据，仅用于日志/诊断 |

`trigger=arrive` 在进入目标页时触发，不依赖离开目标页的翻页信号；默认关闭，仅用于自动播放实验。`onFailure=strict` 保持旧脚本的一次性 done 语义；`self-heal` 清除 done 允许重试；`safe` 立即进入 SafeMode；`legacy` 切换旧 VBS/PowerShell 流程。

### 8.6 Legacy 配置

```json
{
  "version": 1,
  "legacy": {
    "deckName": "deck-live.pptx",
    "targetSlides": [5, 6, 8],
    "srcNames": ["page1.html", "page2.html", "page3.html"],
    "targetHtmls": ["page1.html", "page2.html", "page3.html"],
    "targetProfiles": ["tef-live-page1", "tef-live-page2", "tef-live-page3"],
    "targetLabels": ["Page1", "Page2", "Page3"]
  }
}
```

- 加载时转换为 `targets`，数组等长校验与当前一致。
- `targetHtmls` 映射为 `stageName`，`srcNames` 为源 HTML，`targetProfiles` 为 `edgeProfile`。
- `legacy` 与 `targets` 同时出现时，`targets` 优先；未覆盖的 legacy 项继续生效。

### 8.7 校验、deck 指纹与热加载

- `--validate`：一次性列出所有错误与警告，包含字段路径。
- 校验项：JSON schema、版本、数组等长、页号范围、标题重名、HTML 存在、路径越权、edgeProfile 重复、stageName 冲突、未知字段。
- deck 指纹：路径、大小、修改时间、页数、标题集合；写入 journal。
- deck 变化时重解析标题映射；页数不足或目标页移动时告警。
- `--resolve`：不启动演示，输出最终映射表与解析警告。
- `--watch-config`：只在 Monitoring 安全点热加载；原子交换；非法配置保留旧版本。
- `--print-schema`：输出 JSON Schema；`--init-config`：扫描 deck 生成模板。

### 8.8 CLI

```text
htmlppt-watchdog.exe [--config watcher.json] [--profile NAME] [--list-profiles]
                     [--host=wps|powerpoint] [--scheduling=adaptive|aggressive|balanced|safe]
                     [--stage-mode=file|tree|direct] [--state-dir PATH]
                     [--legacy-fallback=auto|force|off]
                     [--allow-outside-folder] [--watch-config]
                     [--on-supervisor-loss=continue|cleanup] [--on-resume=resume|restart|cleanup]
                     [--focus-strategy=auto|attach-alt|switch] [--topmost-guard=on|off]
                     [--cpu-budget=N] [--fault=NAME] [--fresh-profile] [--poll-only]
                     [--test] [--dry-run] [--self-test] [--status] [--diagnostics]
                     [--stop] [--print-schema] [--print-config] [--validate] [--resolve]
                     [--init-config] [--config-ui]
                     [--map SLIDE=HTML]... [--target JSON]...
```

- `--map` 可重复，优先级最高。
- `--target` 接受一段 JSON，用于临时覆盖单个 target。
- `--profile` 与 `--config` 共同选择配置；`--list-profiles` 列出可用 profile。

### 8.9 示例

最小配置：

```json
{
  "version": 1,
  "deck": "deck-live.pptx",
  "targets": [
    { "slide": 5, "html": "page1.html" },
    { "slide": 6, "html": "page2.html" }
  ]
}
```

带资源目录、显示器与冷启动预热：

```json
{
  "version": 1,
  "deck": "deck-live.pptx",
  "defaults": {
    "scheduling": "adaptive",
    "cover": { "maxHoldMs": 10000 },
    "edge": { "prewarmProfile": true }
  },
  "targets": [
    {
      "slide": 5,
      "html": "pages/audit/index.html",
      "assets": { "mode": "tree", "root": "pages/audit" },
      "label": "Audit",
      "edgeProfile": "tef-live-audit",
      "display": { "monitor": 2 }
    }
  ]
}
```

---

## 9. 原生配置 UI

### 9.1 定位

- `--config-ui` 启动独立 Win32 配置器；与 watchdog 共用 core 配置模型和 `watcher.json`。
- UI 是模型的视图；手写 JSON、生成器模板、UI 保存文件可往返，不丢字段。
- Model-View 分离：`ConfigEditorModel` 在 core，单元测试覆盖；视图只负责交互与渲染。
- UI 可在 watchdog 运行时打开；保存后在 Monitoring 安全点热加载；处于切换/返回阶段时提示延后生效。

### 9.2 主界面

- 左侧：配置 profile/deck 选择器、幻灯片列表（页号、标题、映射 HTML、启用状态、校验状态）、搜索与过滤。
- 右侧：当前页详情：
  - 定位方式：页号 / 标题精确 / 标题前缀 / 标题正则 / 手动条目。
  - HTML：文件/目录选择器、资源模式、assetRoot、include/exclude。
  - 运行参数：label、Edge profile、触发方式、显示器、黑场时序、Edge 参数、冷启动预热、失败策略、启用开关。
  - 校验结果与最终解析路径。
- 工具栏：打开 deck、加载/保存配置、自动映射向导、添加/删除/复制 target、撤销/重做、解析预览、校验、Dry run、启动 watchdog、停止。
- 底部状态栏：deck 指纹、页数、target 数、配置路径、watchdog 状态、热加载状态。
- 预览：v1 提供“在 Edge 中预览”按钮，使用临时 Edge profile；WebView2 内嵌预览为 v2 预留。

### 9.3 自动映射向导

- 选择 deck → COM 扫描页数与标题 → 选择 HTML 根目录 → 预览配对 → 用户确认后写入 targets。
- 一次性辅助策略：
  - 按文件名页号匹配（如 `slide-05.html`）。
  - 按文件列表顺序与选中页顺序配对。
  - 手动逐页选择/拖拽。
- 所有自动结果必须用户确认；运行时不做文件名猜测。
- deck 变化后重新扫描：显示新增/删除/移动页；标题匹配自动迁移，页号变化明确告警。

### 9.4 编辑与安全

- 命令模式撤销/重做；批量操作一次撤销。
- 原子保存：临时文件 → flush → `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)`；保留 `.bak`。
- 保存前完整校验；错误时禁止保存为正式配置，可另存 `.draft.json`。
- 相对路径相对配置文件解析；默认禁止目录外 HTML。
- JSON 稳定键顺序，便于 diff；保留 `version` 并支持迁移。
- COM 不可用时降级为手工页号编辑，不阻塞配置。

### 9.5 技术实现

- 纯 Win32 + Common Controls v6 + DWM，不引入 MFC/ATL，保持 MinGW 可编译。
- PerMonitorV2 DPI、暗色模式、键盘导航、高对比度；中文资源用 `LoadStringW`，源码 UTF-8。
- COM 读取在后台 STA 线程；UI 线程只接收进度/结果；支持取消。
- `IFileOpenDialog` / `IFileSaveDialog`；支持拖拽 HTML/目录。
- 幻灯片缩略图通过 COM 导出临时 PNG；探测失败降级标题文本。
- UI 崩溃不影响已运行的 watchdog；watchdog 崩溃不阻塞 UI 编辑。

### 9.6 与 watchdog 交互

- “启动 watchdog”以独立进程启动 supervisor，默认 detached；UI 通过 `--status`/IPC 显示状态。
- 同一配置已有实例时禁用启动，提供“查看状态 / 停止 / 强制重启”。
- `--dry-run` 只解析、探测、展示动作，不弹黑场、不启动 Edge。
- UI 与 `--init-config` 共用解析/校验核心。

---

## 10. 工程结构

### 10.1 目录

```text
native/
  CMakeLists.txt
  cmake/
  src/
    main.cpp
    app/
      supervisor.cpp/.h
      worker.cpp/.h
      cli.cpp/.h
    core/
      state_machine.cpp/.h
      config.cpp/.h
      journal.cpp/.h
      blackbox.cpp/.h
      clock.cpp/.h
      retry_policy.cpp/.h
      diagnostics.cpp/.h
    platform/win32/
      presentation_host.h
      wps_host.cpp/.h
      com_event_sink.cpp/.h
      overlay.cpp/.h
      focus.cpp/.h
      window_enum.cpp/.h
      process_job.cpp/.h
      ipc.cpp/.h
      crash_dump.cpp/.h
      power.cpp/.h
    compat/
      legacy_flags.cpp/.h
      legacy_launcher.cpp/.h
    configui/
      editor_model.cpp/.h
      win32_view.cpp/.h
      auto_map.cpp/.h
  tests/
    unit/
    integration/
    fakes/
  third_party/
    nlohmann/json.hpp
  resources/
    app.manifest
    version.rc
    icon.ico
```

构建产物写入 `native/build/`，加入 `.gitignore`；不修改现有 `scripts/`、`docs/`、`tests/` 结构。

### 10.2 构建

- CMake + C++20；MSVC 2022 主构建；MinGW-w64 交叉编译 CI。
- 静态 CRT（`/MT`）；MinGW `-static -static-libgcc -static-libstdc++`。
- 单 EXE 便携部署；PDB 单独归档。
- 链接：`ole32 oleaut32 user32 gdi32 shell32 shlwapi psapi dbghelp advapi32`。
- 编译选项：`/W4 /permissive- /utf-8 /guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA`。
- CMake 目标：`htmlppt-watchdog`、`htmlppt-watchdog-tests`、可选 `htmlppt-watchdog-bench`。
- 依赖仅 Win32/COM/STL + vendored nlohmann/json（MIT）；不使用 ATL/WIL。
- COM 使用 late-bound `IDispatch` + 手写 `IConnectionPoint`，不依赖 `#import`。

### 10.3 资源与 manifest

- `asInvoker`、`PerMonitorV2`、`longPathAware`、Windows 10/11 supportedOS。
- 版本资源、图标、SHA256 发布说明。
- 可选 Authenticode 签名步骤；未签名时提供 SHA256 与文档。

### 10.4 日志与诊断

- 兼容日志：`watch-log.txt`、`bring-log.txt`、`cover-log.txt`，写 demo 目录。
- 结构化日志：`journal.jsonl`、`blackbox.jsonl`，写 `--state-dir`。
- `--diagnostics` 打包：配置快照、journal、黑匣子、日志、系统信息（OS build、DPI、显示器、Edge/WPS 版本）、崩溃 dump 清单。
- 日志有界滚动；磁盘不足回退 `%TEMP%`。

---

## 11. 测试与验收

### 11.1 测试矩阵

| 层级 | 内容 | 环境 | 门禁 |
|---|---|---|---|
| Core 单元 | 配置、数组等长、UTF-8、CLI 优先级、热加载拒绝；状态机全部迁移与守卫；done/ignore/return/timeout；重试/退避/熔断；journal 恢复；清理顺序；调度间隔选择 | Linux + Windows | 每次提交 |
| 协议兼容 | 标志文件同名同义、run-id 归属、日志关键字段、`stop-live.vbs` shim、legacy CONFIG 映射 | Windows | 每次提交 |
| 集成（fake） | 假 PresentationHost 脚本化翻页/COM 462/慢启动；假 overlay 握手；假 focus/Edge；supervisor 杀 worker、挂起、管道断开、重启预算、dump helper | Windows | 每次提交 |
| 配置 UI | `ConfigEditorModel` 单元测试；Win32 视图手工验收（拖拽、DPI、暗色、原子保存、热加载、校验面板） | Windows | 合并前 |
| 故障注入 | `com-hang`、`com-462`、`edge-slow`、`cover-hang`、`focus-deny`、`supervisor-loss`、`sleep-resume`、`multi-monitor` | Windows | 合并前 |
| 性能 | 事件处理 p99、触发到全黑、返回恢复、空闲 CPU、aggressive 峰值 CPU、黑场抖动 | Windows 实机 | 合并前 |
| 端到端 | WPS 12.1 + Edge 真机：触发/返回/Alt+F4/Esc/冷启动/睡眠/双屏/DPI/多目录 | 目标机 | 发布前 |
| 构建 | MSVC Release/Debug、MinGW 交叉编译、静态 CRT、manifest、版本资源 | CI | 每次提交 |

- 现有 `npm test` 保持通过。
- Windows CI 使用 fake host 跑集成测试；WPS/Edge 端到端保留人工验收清单。
- 所有失败场景必须有可重复 fault 开关。

### 11.2 验收标准

行为：

- deck、HTML、演示流程零修改；触发、去重、返回、测试模式与当前一致。
- 标志文件与关键日志词可被旧工具识别；`stop-live.vbs` 与 `--stop` 都能停止并清理。
- 同目录单实例；不同目录可并行且互不影响。

韧性：

- 在 Monitoring/Live/Returning 任意阶段杀 worker：supervisor 重启，journal 对账，无残留黑场/Edge，状态可继续或安全退出。
- COM 线程卡死：可取消则恢复，不可取消则进程级重启，5 秒内给出结果。
- Edge 冷启动慢：黑场保持到 `edgeReady` 或安全上限；正常冷启动不露编辑器。
- 聚焦子系统崩溃：不影响 Edge 关闭检测与返回主流程。
- 两个 demo 目录同时运行：停止一个不误杀另一个。
- 睡眠/唤醒、显示器热插拔、DPI 变化：无黑屏残留，定时器重新校准。
- 连续崩溃超过预算：进入 SafeMode，隐藏黑场、恢复 WPS、输出诊断，可选 legacy 回退。

性能：

- 触发到全黑 p99 < 350ms；事件处理 p99 < 1ms（关键窗口）；Edge/返回事件判定 p99 < 10ms。
- balanced 空闲 CPU < 1%；aggressive 插电峰值 < 5%；电池默认 balanced。

可观测性：

- `--status` 输出状态、run-id、PID、最后事件。
- `--diagnostics` 输出完整诊断包。
- 崩溃有 dump 与黑匣子快照。

### 11.3 兼容验收

- 使用现有 `templates/stepwise-demo.html` 或演示 HTML，不改动内容。
- 使用现有 `watch-live-content.vbs` 的 CONFIG 值生成 legacy JSON，行为一致。
- 旧 `stop-live.vbs` 可停止新实例；`--stop` 可停止旧流程遗留的 Edge/helper（尽力而为）。
- 旧日志阅读习惯保留；新增 JSONL 不替代人类可读日志。

---

## 12. 交付阶段

1. P0：CMake 骨架、core 接口、配置模型、时钟、journal、单元测试。
2. P1：supervisor/worker、Job、IPC、心跳、崩溃转储、黑匣子。
3. P2：WPS host、状态机、兼容标志、日志、stop 入口。
4. P3：overlay、focus、Edge Job/窗口检测、失败策略。
5. P4：事件驱动、调度分级、电源策略、性能插桩。
6. P5：配置 UI、自动映射向导、故障注入、CI、端到端验收、文档与发布包。

每个阶段都必须保持现有 `npm test` 通过，并新增对应 native 测试。

---

## 13. 预留扩展

以下项目不在 v1 实现，但接口与配置字段预留：

- PPT 备注标签 `@html:` 与文件名约定运行时自动发现。
- WebView2 内嵌 HTML 预览。
- PowerPoint 完整宿主实现与验收矩阵。
- 同一目标页多 HTML 序列（`TEF-LIVE-NEXT`）。
- 登录自启动（计划任务/启动项）。
- 双 worker 热备。
- 远端遥测与集中诊断。
- Web 配置编辑器。

预留方式：

- `PresentationHost` 接口与 `--host` 开关。
- `discover` 配置段。
- `sequence` 字段。
- `--on-supervisor-loss`、`--legacy-fallback`、`--scheduling` 等策略开关。
- 配置 `version` 与迁移函数。

---

## 14. 附录

### 14.1 标志文件兼容映射

| 标志 | 写入方 | 读取方 | 说明 |
|---|---|---|---|
| `watcher.lock` | worker | 兼容层/旧工具 | JSON 行含 run_id/pid/config_hash；5 秒刷新 |
| `cover-fade.flag` | worker | overlay 兼容层 | 请求一次渐黑 |
| `cover-ready.flag` | worker | overlay 兼容层 | 内容就绪，允许淡出 |
| `cover-done.flag` | overlay | worker/旧工具 | 序列完成 |
| `cover-stop.flag` | worker | overlay | 退出 |
| `cover-hide.flag` | worker | overlay | 立即隐藏 |
| `agent-arm.flag` | worker | 兼容层/旧工具 | `profile\|exitFlag\|readyFlag` |
| `agent-stop.flag` | worker | 兼容层/旧工具 | 停止 |
| `return-request.flag` | 聚焦线程/兼容层 | worker | 返回请求 |
| `edge-closed.flag` | 聚焦线程/兼容层 | worker | Edge 关闭 |

### 14.2 默认时间参数

| 参数 | 默认值 |
|---|---|
| 黑场渐黑 | 300ms |
| 黑场最短保持 | 1000ms |
| 黑场安全上限 | 10000ms |
| 启动忽略窗口 | 1s |
| 返回忽略窗口 | 2s |
| Edge ready 超时 | 20s |
| Edge 最长存活 | 600s |
| 聚焦单次超时 | 6s |
| COM 错误窗口 | 2s |
| supervisor 心跳 | 200ms |
| supervisor 心跳超时 | 2s |
| 重启预算 | 10 分钟 5 次 |
| 重启退避 | 200ms / 500ms / 1s / 2s / 5s |
| 连续触发失败阈值 | 3 |
| worker 线程心跳检查 | 100ms |
| worker 线程心跳超时 | 1s |
| 黑匣子环形缓冲 | 8 MB |
| 空闲轮询（adaptive） | 10ms |
| 关键窗口轮询（adaptive） | 1ms |

### 14.3 术语

- `supervisor`：轻量监控进程，负责 Job、心跳、重启、转储。
- `worker`：业务进程，承载状态机、COM、overlay、focus、Edge。
- `cover`：全屏黑场。
- `doneFlags`：本场放映已触发的目标页集合。
- `run_id`：一次 supervisor/worker 会话的唯一标识，用于状态、标志、进程归属。
- `journal`：状态迁移的追加日志。
- `black box`：共享内存环形诊断缓冲。
- `legacy`：现有 VBS + PowerShell 实现。
- `config profile`：`watcher.json` 中 `profiles` 下的一个演示配置。
- `Edge profile`：每个目标页独立 `--user-data-dir` 的标识，字段名为 `edgeProfile`。
