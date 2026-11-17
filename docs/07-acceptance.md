# 端到端验收清单

对应设计规格 §11.2（验收标准）与 §11.3（兼容验收）。每条给出**验证命令**和**通过判据**；
标 `[实机]` 的条目必须在装有 WPS + Edge 的目标机上执行，其余可在任意 Windows 机器上做。

准备：

```powershell
python demo/make_deck.py
native\build\htmlppt-watchdog.exe --validate --config demo\watcher.json
```

## 一、行为

| # | 项目 | 命令 | 通过判据 |
|---|---|---|---|
| 1 | deck/HTML/演示流程零修改 | 用现有 deck 与 `templates/live-page.html` 跑一次 | 触发、返回与 VBS 版观感一致，HTML 文件未被修改 |
| 2 | 旧标志与日志词可识别 `[实机]` | 运行中 `Get-Content demo\watch-log.txt -Tail 5`；`Test-Path demo\watcher.lock` | 运行期存在 `watcher.lock`；`watch-log.txt` 含启动/触发/返回/cleanup 行 |
| 3 | `--stop` 停止并清理 `[实机]` | `--supervisor ...` 后另开终端 `--stop` | 退出码 0；放映窗口数 0；`%TEMP%\htmlppt-<run-id>` 被删除；`watcher.lock` 被删除 |
| 4 | 同目录单实例 | 同 `--state-dir` 起第二个 supervisor | 第二个立即退出并提示已在运行 |
| 5 | 不同目录可并行 | 两个 state-dir、两个 run-id 同时跑 | 互不影响；`--stop` 一个不影响另一个 |

## 二、韧性

| # | 项目 | 命令 | 通过判据 |
|---|---|---|---|
| 6 | 任意阶段杀 worker `[实机]` | 运行中 `Stop-Process -Name htmlppt-watchdog -Id <worker>` | supervisor 重启 worker，`journal.jsonl` 有 restart 记录，无残留黑场/Edge |
| 7 | COM 卡死恢复 | `set HTMLPPT_FAULTS=com-hang` 后运行 | 2 s 错误窗口到期后进入 SafeMode（或重启）；`--status` 可见状态迁移 |
| 8 | Edge 冷启动慢 `[实机]` | `set HTMLPPT_FAULTS=edge-slow` 后触发 | 黑场保持到 `edge ready`，不露编辑器；3 s 内不判定就绪 |
| 9 | 连续崩溃超预算 | 反复杀 worker 超过预算 | 进入 SafeMode；黑场隐藏、放映退出、WPS 可用；`--status` 出现 `[SafeMode]` |
| 9b | 故障开关可见 | `set HTMLPPT_FAULTS=edge-slow` 后启动 | `--status` 出现 `faults: edge-slow`；注入运行在日志中可见 |
| 10 | 睡眠/唤醒 `[实机]` | 运行中睡眠再唤醒 | 覆盖窗口重新铺满并置顶，无黑屏残留（截止时间不重新校准，见第六节） |

## 三、性能

| # | 项目 | 命令 | 通过判据 |
|---|---|---|---|
| 11 | 触发到全黑 `[实机]` | `set HTMLPPT_BENCHMARK=1` 运行后看 `benchmark.txt` | step p99 与 com p99 有实测数值；触发到全黑 p99 < 350 ms |
| 12 | 空闲 CPU | 任务管理器观察运行中的 watchdog | 空闲 < 1%（当前 100 ms 轮询档） |

## 四、可观测性

| # | 项目 | 命令 | 通过判据 |
|---|---|---|---|
| 13 | 状态/run-id/PID/最后事件 | `--status --run-id <id> --state-dir <dir>` | 首行含 `state <Name> pid <n>` 与 `last event ...` |
| 14 | 诊断包 | `--diagnostics --run-id <id> --state-dir <dir> --out diag` | 生成 `manifest.txt`、journal、watch-log、plan/config；有 dump 时一并收集 |
| 15 | 崩溃 dump | `--dump-helper <pid> <path>` 或真实崩溃 | 生成 `.dmp`，诊断包会收集 |

## 五、兼容验收（§11.3）

| # | 项目 | 命令 | 通过判据 |
|---|---|---|---|
| 16 | 现有 HTML 不改动 | 用 `templates/stepwise-demo.html` 或现场页面 | 页面无需修改即可用 |
| 17 | legacy CONFIG → JSON | `--print-config` 对比 `watch-live-content.vbs` 的 CONFIG | 目标页、HTML、profile、标签一致 |
| 18 | 旧 `stop-live.vbs` 停止新实例 | 运行中双击 `scripts/stop-live.vbs` | **当前不通过**：见“未实现”一节 |
| 19 | 旧日志阅读习惯 | 查看 `watch-log.txt` | 仍是人类可读行，JSONL 不替代它 |

## 六、未实现（验收时会失败）

这些条目在规格里存在但代码尚未接线，验收时**不应勾选**：

- 睡眠/唤醒、显示器热插拔、DPI 变化后的**定时器重算**（覆盖窗口已在 `WM_DISPLAYCHANGE`、`WM_DPICHANGED`、`WM_POWERBROADCAST` 时重新铺满并置顶，但截止时间不重新校准）。
- 旧 `stop-live.vbs` 与 `--stop` 互操作（新实现只写 `watcher.lock`，不监听其删除）。
- CPU 占用的自动化测量（`benchmark.txt` 已给出 step/com 的 p50/p99，CPU 仍需人工观察）。
- `sleep-resume`、`multi-monitor` 两个故障开关（其余六个已可用，见 `native/README.md` 的 Fault injection）。
- `focus.strategy`、`display.monitor`、`edge.prewarmProfile` 仍只解析不生效（`tree` 的 `include`/`exclude` glob 已实现）。
- 配置 UI（§9）：`ConfigEditorModel` 已实现并有单测（增删目标、校验、原子保存），Win32 视图与自动映射向导未实现。
