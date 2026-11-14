# 复现指南

> 状态：操作手册
> 日期：2026-11-14
> 关联：[架构](01-architecture.md)、[实验与发现](02-experiments-and-findings.md)、[playbook](05-playbook.md)

## 1. 环境

### 模式 A（捕获/编码/组装，Linux 或 Windows 均可）

| 依赖 | 说明 |
|------|------|
| Node.js 24+ | `capture.mjs` / `encode.mjs` 使用内置 fetch/WebSocket |
| Google Chrome / Chromium | 无头捕获；可用 `CHROME_BIN` 指定 |
| ffmpeg / ffprobe | 编码与校验；可用 `FFMPEG_BIN` / `FFPROBE_BIN` 指定 |
| Python 3.12+ | 组装/校验脚本 |
| python-pptx 1.0.2 | 已在 `vendor/`；`PYTHONPATH=vendor` |
| Pillow | `verify.py` / `wps_smoke.ps1` 需要 |
| CJK 字体 | 默认 `Microsoft YaHei`；`REQUIRED_FONT` 可覆盖 |

### 模式 B/B2（Windows 现场）

| 依赖 | 说明 |
|------|------|
| Windows 10/11 | 目标环境 |
| WPS Office | 实测 12.1.0.29690；`Kwpp.Application` COM |
| Microsoft Edge | `--app` 全屏应用窗口 |
| Windows PowerShell | 5.1+；脚本使用 `-ExecutionPolicy Bypass` |

## 2. 模式 A 复现

### 2.1 预检

```bash
bash scripts/preflight.sh
```

如果 `python-pptx` 不在 `vendor/`：

```bash
bash scripts/setup.sh
```

### 2.2 HTML 契约

`templates/stepwise-demo.html` 是 2 步确定性样例。检查契约：

```bash
node scripts/check-contract.mjs --html templates/stepwise-demo.html
```

真实页面必须暴露：

```js
window.__tefSteps = {
  total: 8,
  durations: [1800, 3200, 3200, 3200, 3200, 3200, 3200, 2800],
  seek(step, tMs) {}
};
```

`?capture=1` 下必须关闭自动播放/键盘/鼠标/视差，`seek` 同步、可重复、无真实时间依赖。

### 2.3 捕获

```bash
node scripts/capture.mjs \
  --html templates/stepwise-demo.html \
  --out out/capture \
  --width 1920 --height 1080 --fps 60
```

### 2.4 编码

```bash
node scripts/encode.mjs \
  --manifest out/capture/manifest.json \
  --out out/clips --crf 14
```

### 2.5 WPS 压缩（目标为 WPS 时必做）

```bash
node scripts/compress-clips.mjs \
  --manifest out/clips/manifest.json \
  --out out/clips-wps --max 0.9
```

### 2.6 校验

```bash
python3 scripts/verify.py \
  --capture out/capture/manifest.json \
  --clips out/clips/manifest.json
```

### 2.7 组装 PPTX

```bash
PYTHONPATH=vendor python3 scripts/build_pptx.py \
  --source deck.pptx \
  --clips out/clips-wps/manifest.json \
  --after 12 --mode native \
  --out deck-animated.pptx
```

> 本仓库不附带示例 deck；请使用自己的 PPTX，并按实际页数调整 `--after`。

### 2.8 结构校验

```bash
PYTHONPATH=vendor python3 scripts/verify_pptx.py \
  --source deck.pptx --out deck-animated.pptx \
  --new-index 13 --expected-slides 20 --expected-videos 8 --mode native
```

### 2.9 现场冒烟

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File scripts/wps_smoke.ps1 -Path deck-animated.pptx -Out preview.png
```

冒烟只验证“能打开、首帧正确”，不验证放映态空格分步；必须在 WPS/Office 放映态人工验收。

## 3. 模式 B/B2 复现（Windows）

### 3.1 文件布局

```text
demo/
  deck.pptx
  watch-live-content.vbs
  focus-agent.ps1
  black-fade.ps1
  bring-wps.ps1
  focus-lib.ps1
  test-focus.ps1
  stop-live.vbs
  page1.html
  page2.html
  page3.html
```

所有文件必须同目录；不要只移动 PPTX。

### 3.2 配置

编辑 `watch-live-content.vbs` 顶部 CONFIG：

```vbs
deckName = "deck.pptx"
targetSlides = Array(5, 6, 8)
srcNames = Array("page1.html", "page2.html", "page3.html")
targetHtmls = Array("page1.html", "page2.html", "page3.html")
targetProfiles = Array("tef-live-page1", "tef-live-page2", "tef-live-page3")
targetLabels = Array("Page1", "Page2", "Page3")
```

中文文件名用 `ChrW` 构造，例如 `审计.html`：

```vbs
srcNames = Array(..., ChrW(&H5BA1) & ChrW(&H8BA1) & ".html")
targetHtmls = Array(..., "audit.html")
```

### 3.3 聚焦自检

```powershell
powershell.exe -ExecutionPolicy Bypass -File test-focus.ps1
```

看到 `PASS: foreground activation works` 再进行放映。

### 3.4 启动

双击 `watch-live-content.vbs`；首次如提示安全警告，选择允许。

验收模式：

```powershell
wscript.exe watch-live-content.vbs /test
```

`/test` 会跳到 `targetSlides(0)`，用于快速验证第一个目标页；正式演示不要带 `/test`。

### 3.5 操作

- 进入目标页不会自动跳转；
- 按一次空格触发对应 HTML；
- HTML 内空格推进，最后一步空格或 Esc 返回；
- 返回后继续按空格进入下一页，同一页不会重复触发；
- 应急停止：双击 `stop-live.vbs`。

### 3.6 日志

| 文件 | 内容 |
|------|------|
| `watch-log.txt` | 启动、触发页、返回请求、恢复、COM 错误、cleanup |
| `bring-log.txt` | agent 会话、窗口查找、聚焦结果 |
| `cover-log.txt` | 黑场 fade in / hold / fade out / done |

### 3.7 Edge 同步弹窗

启动参数已包含 `edgeFlags`。如果旧 profile 已出现同步弹窗：

```powershell
Remove-Item "$env:TEMP\tef-live-*" -Recurse -Force
```

让 watcher 用全新 profile 重新创建。仍不行时使用策略兜底（全局，需管理员）：

```reg
[HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\Edge]
"SyncDisabled"=dword:00000001
"ImplicitSignInEnabled"=dword:00000000
"HideFirstRunExperience"=dword:00000001
```

## 4. 验收清单

### 模式 A

- [ ] `preflight.sh` 通过（字体/Chrome/ffmpeg/Pillow/python-pptx）。
- [ ] `check-contract.mjs` 通过。
- [ ] 2 步样例边界帧逐像素一致。
- [ ] PPTX 页数符合预期（通常只 +1）。
- [ ] 原页 C14N 不变。
- [ ] WPS/Office 放映态：进页首段播放、空格下一段、最后空格翻页。

### 模式 B/B2

- [ ] 聚焦自检 `PASS`。
- [ ] 目标页进入时不自动跳转；空格才触发。
- [ ] 渐黑完成后才切换（`cover hold` 早于 `edge launched`）。
- [ ] Edge 就绪后才淡出。
- [ ] 返回黑场在 Edge 关闭前启动，WPS 恢复时不露编辑器。
- [ ] 返回后空格继续翻页，不重复触发。
- [ ] 无逐页安全确认。
- [ ] 强制结束 WPS 后自动清理，无残留。
- [ ] 无 Edge 同步弹窗。

## 5. 常见问题

| 现象 | 处理 |
|------|------|
| `FAIL: Could not start WPS` | 关闭残留 WPS/旧 watcher；WPS COM 可能挂起 |
| `Deck not found` | 检查 `deckName`；中文用 `ChrW` |
| Edge 空白页/新标签页 | 确认 `targetHtmls` 是 ASCII 暂存名；不要用 UNC/中文路径 |
| 空格不推进 | HTML 同时判断 `e.code === "Space"` 和 `e.key === " "` |
| 切换时露馅 | 确认 `coverFadeWaitMs=350`；`cover hold` 早于 `edge launched` |
| 返回露 WPS 编辑器 | 检查 HTML 的 `TEF-LIVE-RETURN` 标题信号与 2s 延迟 |
| 黑屏过长 | Edge 启动慢；等待 `cover-ready`；可调 `MaxHoldMs` 但不要提前淡出 |
| 闪退 | 看 `watch-log.txt` 的 `COM busy`；连续 20 次失败会 cleanup；用 `cscript` 复现 VBScript 错误 |
| 同步弹窗 | 删除 `%TEMP%\tef-live-*`；或用 `SyncDisabled`/`ImplicitSignInEnabled` 策略 |
| watcher 卡住 | 双击 `stop-live.vbs`；再按 Esc 退出 WPS 放映 |
