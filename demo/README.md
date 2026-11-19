# 模式 B2 演示目录

一条命令跑通“在目标页按空格 → 黑场 → Edge 打开 HTML → 返回 WPS”。

## 1. 生成 deck 与页面

```powershell
python demo/make_deck.py
```

生成 `demo/deck.pptx`（5 页，16:9）并把 `templates/live-page.html` 复制为
`demo/live-page.html`。两者都是生成物，不入库。

## 2. 自检

```powershell
npm run preflight
native\build\htmlppt-watchdog.exe --validate --config demo\watcher.json
```

`--validate` 应输出 `OK: configuration is valid`。

## 3. 自动化冒烟

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-watchdog.ps1 `
  -Config demo\watcher.json -StateDir demo\state `
  -Exe native\build\htmlppt-watchdog.exe
```

它在 `demo/state` 下起一次真实运行，要求 worker 打开 deck、启动放映并到达
`Monitoring`，然后干净停止。通过时应看到：

```text
OK: run smoke reached state 2 and stopped cleanly
```

## 4. 现场演示（手动）

```powershell
native\build\htmlppt-watchdog.exe --supervisor --config demo\watcher.json --state-dir demo\state --run-id demo
```

1. WPS 打开 deck 并开始放映；
2. 翻到第 3 页（`Live demo`），按一次空格——黑场淡入，Edge 全屏打开
   `live-page.html`；
3. 页面里空格前进、Esc 返回：标题变为 `TEF-LIVE-RETURN`，看门狗在黑场下关闭
   Edge、恢复 WPS 并停在第 3 页；
4. 结束后另开一个终端停止：

```powershell
native\build\htmlppt-watchdog.exe --stop --run-id demo --state-dir demo\state
```

`--stop` 会退出放映但保留 WPS 编辑器（`Release()` 的契约）。

## 文件

| 文件 | 作用 |
|---|---|
| `make_deck.py` | 生成 `deck.pptx` 与 `live-page.html`（用 `vendor/` 里的 python-pptx） |
| `watcher.json` | 配置：目标第 3 页 → `live-page.html`，`assets.mode=file` 走 staging |
| `live-page.html` | 生成物，来自 `templates/live-page.html` |
| `.htmlppt-run/` | 生成物（与 `watcher.json` 同目录）：`worker-plan.json`、`worker-config.json`、`journal.jsonl` |
| `watch-log.txt` | 生成物（与 `watcher.json` 同目录）：worker 的运行时日志 |

排查：`demo/.htmlppt-run/journal.jsonl` 是主管视角的时间线，`demo/watch-log.txt`
是 worker 的运行时日志（含着色、聚焦、返回的重试细节）。
