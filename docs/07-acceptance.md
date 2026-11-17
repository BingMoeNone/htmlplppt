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
| 10 | 睡眠/唤醒 `[实机]` | 运行中睡眠再唤醒 | 覆盖窗口重新铺满并置顶；日志出现 `power resume: shifted deadlines by <n> ms`，无黑屏残留 |

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

## 六、配置 UI（§9）

没有 WPS 的机器加 `--deck-info deck-info.json`（与 `--validate` 相同）。

| # | 项目 | 命令 | 通过判据 |
|---|---|---|---|
| 20 | 缺失配置也能打开 | `native\build\htmlppt-watchdog.exe --config-ui --config missing\watcher.json` | 窗口照常打开而不是退出；状态栏为 `cannot load <path>: <第一条问题>`，列表为空；deck 属于整份配置、窗口中不可编辑，需在窗口外修复文件后重新打开，期间 Save 保持禁用 |
| 21 | 编辑并保存 | 打开有效配置，选中一行，改 LABEL/HTML 后 Apply，再点 Save | 列表与状态栏反映新值；目标文件是新的原子写入结果，旧内容保留为 `<name>.bak`；保存成功后 Save 按钮变灰（模型不再 dirty） |
| 22 | 有错时禁止保存 | 把一行的 slide 改成与另一行重复后 Apply；再用一个 deck 缺失的配置试全局错误 | 状态栏显示首个错误（包括没有对应行的全局错误），Save 按钮置灰，文件不会被写入 |
| 23 | 关闭提示 | 修改后直接关闭窗口，在 Yes/No/Cancel 提示里分别操作 | Yes：保存成功后关闭；保存失败则保留窗口并把错误写进状态栏。No：不保存直接关闭。Cancel：保留窗口与编辑内容 |

## 七、未实现（验收时会失败）

这些条目在规格里存在但代码尚未接线，验收时**不应勾选**：

- 睡眠/唤醒、显示器热插拔、DPI 变化：覆盖窗口会重新铺满并置顶，唤醒后截止时间按休眠间隔平移（`power resume` 日志）；跨屏 DPI 的逐屏重算仍未做。
- 旧 `stop-live.vbs` 与 `--stop` 互操作（新实现只写 `watcher.lock`，不监听其删除）。
- CPU 占用的自动化测量（`benchmark.txt` 已给出 step/com 的 p50/p99，CPU 仍需人工观察）。
- 故障开关已全部可用（`com-hang`、`com-462`、`edge-slow`、`cover-hang`、`focus-deny`、`supervisor-loss`、`sleep-resume`、`multi-monitor`），见 `native/README.md` 的 Fault injection。
- `display.monitor`、`focus.strategy`、`edge.prewarmProfile` 已接线（选屏、激活链选择、Edge 预热）；`tree` 的 `include`/`exclude` glob 已实现。
- 配置 UI（§9）：`--config-ui` 编辑器（列表、详情、工具栏、Save 门禁、原子保存 + `.bak`、关闭提示、DPI 重排）已实现；自动映射向导仍未实现。
