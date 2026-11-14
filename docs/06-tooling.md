> **来源**：html-animation-to-pptx skill 的 README.md。本文是工具链的详细说明；项目结构以本仓库 README 为准。

# html-animation-to-pptx

把动画 HTML 页面嵌入 PowerPoint/WPS，做成单页、空格分步、保留过程动效的视频动画。

## 安装

项目级（随仓库版本化）：

```text
<repo>/.claude/skills/html-animation-to-pptx/
```

用户级（所有项目可用）：

```bash
cp -r .claude/skills/html-animation-to-pptx ~/.claude/skills/
```

从分享压缩包安装：

```bash
unzip html-animation-to-pptx-skill.zip -d /tmp
cp -r /tmp/html-animation-to-pptx ~/.claude/skills/
```

压缩包已包含 `vendor/`（python-pptx 1.0.2），通常可直接使用；若目标 Python 版本不兼容，重新运行 `bash scripts/setup.sh`。

依赖：

- Google Chrome（无头捕获）
- Node 24+（内置 WebSocket/fetch，无需 npm 依赖）
- Python 3 + Pillow
- ffmpeg / ffprobe（可用 `FFMPEG_BIN` / `FFPROBE_BIN` 指定）
- fontconfig + 目标字体（默认 Microsoft YaHei）
- python-pptx（`scripts/setup.sh` 安装到 `vendor/`）

安装与预检：

```bash
bash scripts/setup.sh
bash scripts/preflight.sh
```

## 快速开始（2 步样例）

```bash
# 1. 准备一个至少 1 页的源 PPTX，例如 deck.pptx
# 2. 预检 HTML 契约
node scripts/check-contract.mjs --html templates/stepwise-demo.html

# 3. 捕获 2 步确定性样例
node scripts/capture.mjs --html templates/stepwise-demo.html --out out/capture

# 4. 编码
node scripts/encode.mjs --manifest out/capture/manifest.json --out out/clips

# 4b. WPS 目标：把每段压到 ≤0.9 秒（转场 + 末帧保持；Office-only 可跳过）
node scripts/compress-clips.mjs --manifest out/clips/manifest.json --out out/clips-wps --max 0.9

# 5. 校验边界帧（用原始 clips manifest）
python3 scripts/verify.py --capture out/capture/manifest.json --clips out/clips/manifest.json

# 6. 组装（WPS 目标用 out/clips-wps/manifest.json；插到第 1 页之后，native 模式）
PYTHONPATH=vendor python3 scripts/build_pptx.py \
  --source deck.pptx --clips out/clips/manifest.json \
  --after 1 --mode native --out deck-animated.pptx

# 7. 结构校验（2 步样例：总页数 = 源页数 + 1）
PYTHONPATH=vendor python3 scripts/verify_pptx.py \
  --source deck.pptx --out deck-animated.pptx \
  --new-index 2 --expected-slides 2 --expected-videos 2 --mode native
```

捕获分辨率必须与源 PPT 的页面宽高比一致（16:9 → 1920×1080，4:3 → 1440×1080）；`build_pptx.py` 会读取 deck 的实际页面尺寸，并拒绝宽高比不匹配的片段，避免视频被拉伸或溢出。

然后在 WPS/Office 中放映目标页，按空格逐步验证。

## 模式 B：实时浏览器（WPS 兼容）

当视频分步在 WPS 上因单段时长失败，或需要完整交互时：

1. 生成自动启动页（无按钮）：

```bash
PYTHONPATH=vendor python3 scripts/build_auto_deck.py \
  --source deck.pptx --after 12 --out demo/deck-live.pptx \
  --title "六条设计公理 · 实时演示" \
  --subtitle "切换到本页后，浏览器动画将自动启动"
```

2. 同一文件夹放：`deck-live.pptx`、`watch-slides.vbs`、`bring-edge.ps1`、`bring-wps.ps1`、`focus-lib.ps1`、`black-cover.ps1`、`launch-animation.vbs`、`V2.html`。
3. HTML：`<title>TEF-LIVE-STAGE</title>`；Esc 和最后一步后的 Space 调 `window.close()`。
4. 运行预检：

```powershell
powershell.exe -File scripts/preflight-live.ps1 -Folder demo
```

5. 目标机聚焦自检：

```powershell
powershell.exe -ExecutionPolicy Bypass -File scripts/test-focus.ps1
```

看到 `PASS` 再进行放映验收；测试会短暂切换焦点并自动还原。

6. 双击 `watch-slides.vbs`：启动 WPS、打开 deck、从第 1 页放映；到目标页退出放映，再启动 Edge 并自动聚焦；Edge 关闭后 `Run()+FocusShow+GotoSlide(13)` 回到第 13 页并自动聚焦，用户再按空格到第 14 页。
7. 首次运行监视器可能提示一次；无逐页提示。

无法运行监视器时退回按钮模式：

```bash
PYTHONPATH=vendor python3 scripts/build_launcher_deck.py \
  --source deck.pptx --after 12 --launcher launch-animation.vbs \
  --out demo/deck-live.pptx \
  --title "六条设计公理 · 实时演示" \
  --button "点击启动动画（Esc 返回）"
```

模板：`scripts/watch-slides.vbs`、`scripts/watch-live-content.vbs`、`scripts/focus-agent.ps1`、`scripts/black-fade.ps1`、`scripts/bring-edge.ps1`、`scripts/bring-wps.ps1`、`scripts/focus-lib.ps1`、`scripts/test-focus.ps1`、`scripts/stop-watch.vbs`、`scripts/stop-live.vbs`、`scripts/launch-edge-app.vbs`、`scripts/build_auto_deck.py`、`scripts/preflight-live.ps1`；详见 `references/live-browser-mode.md` 与 `references/live-content-trigger.md`。

如果要在**现有内容页**上按空格触发、不新增页、不加按钮，使用 `scripts/watch-live-content.vbs` + `scripts/focus-agent.ps1` + `scripts/black-fade.ps1` + `scripts/stop-live.vbs`；配置、HTML 契约、黑场/返回信号与验证步骤见 `references/live-content-trigger.md`。

## 真实页面接入

1. 让 HTML 实现 `window.__tefSteps = { total, durations, seek(step, tMs) }`。
2. `?capture=1` 下关闭交互与实时依赖，保证边界帧一致。
3. 先取真实相邻 2 步做样例，验证字体、边界、模式、现场播放。
4. 通过后再全量捕获与组装。

契约细节见 `references/html-contract.md`；PPTX 时序见 `references/pptx-timing.md`；现场兼容与降级见 `references/compatibility.md`；现有内容页空格触发见 `references/live-content-trigger.md`。

## 目录

```text
html-animation-to-pptx/
  SKILL.md
  README.md
  scripts/
    preflight.sh
    setup.sh
    capture.mjs
    encode.mjs
    verify.py
    build_pptx.py
    verify_pptx.py
    check-contract.mjs
    wps_smoke.ps1
    compress-clips.mjs
    launch-edge-app.vbs
    build_launcher_deck.py
    watch-slides.vbs
    watch-live-content.vbs
    focus-agent.ps1
    black-fade.ps1
    stop-live.vbs
    bring-edge.ps1
    bring-wps.ps1
    focus-lib.ps1
    test-focus.ps1
    black-cover.ps1
    stop-cover.ps1
    stop-cover.vbs
    stop-watch.vbs
    build_auto_deck.py
    preflight-live.ps1
    lib/
  references/
  templates/
```

## 与仓库参考实现的关系

本 skill 的脚本来自 LyMtrix-TEF 的 `docs/competition/html2ppt/`，并做了通用化：

- `FFMPEG_BIN` / `FFPROBE_BIN` 可覆盖编码器路径；
- `REQUIRED_FONT` 可覆盖字体检查；
- `capture.mjs` 增加字体预检；
- `verify.py` 不再依赖固定仓库路径。

如果仓库内已有更新版本，优先使用仓库版本；本 skill 适合跨项目复用。
