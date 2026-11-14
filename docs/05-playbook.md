> **来源**：html-animation-to-pptx skill 的 SKILL.md。本文保留完整操作流程，作为本项目的 playbook；项目内路径以 `scripts/`、`references/` 为准。

# HTML 动画嵌入 PPTX（视频分步 / 实时浏览器）

## 核心原理

PowerPoint/WPS 没有可依赖的 HTML 运行时。两条可落地路线：

**模式 A：单页分步视频**。把 HTML 动画拆成 N 个确定性步骤，每步渲染成一段视频，全部叠在同一张幻灯片上，用原生单击动画逐段播放。无宏、离线；适合 2 秒以内的短片段。

**模式 B：实时浏览器启动**。幻灯片按钮通过相对超链接启动本地 VBS，VBS 用 Edge 全屏应用模式打开同目录的 HTML。完整保留 HTML/CSS/JS 与交互；适合 WPS 单页长视频失效、或需要现场交互的场景。不是单文件方案，首次点击需允许安全提示。

- 模式 A：进入页面第 1 段自动播放；空格播放下一段；最后一段后空格翻页；只新增 1 页。
- 模式 B：点击按钮启动 Edge；空格在 HTML 内步进；Esc/Alt+F4 返回 PPT；PPT 页数 +1。

## 何时使用

适用：

- HTML 动画要放进 PPT，且必须保留过程动效
- 页数有严格上限，不能一步一页
- 现场是 Windows PowerPoint/WPS，不允许宏或联网插件
- 需要可重复渲染、可局部重渲染、可审计的产物

不适用：

- 需要现场鼠标/键盘交互的活页面 → ActiveX/VBA 或 Web 加载项（需单独验证现场环境）
- 只需要静态画面 → 高 DPI 截图或 PDF 矢量导出
- 不需要分步、也不改稿 → 直接录一段 MP4 更省事

## 决策表

| 需求 | 路线 |
|------|------|
| 保留动效 + 空格分步 + 页数受限 + 片段 ≤1 秒 | 模式 A：单页分段视频 + 原生单击动画 |
| WPS 单页视频 >1 秒点击失效 | 模式 B：实时浏览器（相对超链接 + Edge app） |
| 必须保留完整 HTML 交互 | 模式 B：实时浏览器 |
| 保留动效但不要求分步 | 单段 MP4，进页自动播放 |
| 只要静态高清图 | 1920/2560 宽 PNG，或 PDF→矢量 |
| 必须单文件、不能带外部 HTML/启动器 | 模式 A；或降级静态图 |

## 实施流程

### 0. 预检（必须）

```bash
bash scripts/preflight.sh
bash scripts/setup.sh   # 首次安装 python-pptx 到 skill/vendor
```

字体是硬门槛：`fc-match "$REQUIRED_FONT"` 必须命中目标字体（默认 Microsoft YaHei）。字体不对时捕获会产出整批错字帧，**必须在捕获前失败**，不能靠事后检查。

同时确认源 PPT 的页面宽高比：捕获分辨率必须与 deck 一致（16:9 → 1920×1080，4:3 → 1440×1080）。`build_pptx.py` 会读取 deck 的实际 `slide_width` / `slide_height`，并拒绝宽高比不匹配的片段。

### 1. HTML 契约

页面必须暴露：

```js
window.__tefSteps = {
  total: 8,
  durations: [1800, 3200, 3200, 3200, 3200, 3200, 3200, 2800],
  seek(step, tMs) {}
};
```

`?capture=1` 时必须：关闭自动播放与键盘/鼠标/点击/视差；`seek` 同步、可重复、无真实时间依赖；相邻步骤边界帧逐像素一致；全局 CSS keyframe 用 Web Animations API 定位；步骤局部的淡入/印章动画关闭；CSS transition 禁用。

接入真实页面前先跑契约预检：

```bash
node scripts/check-contract.mjs --html page.html
```

详见 `references/html-contract.md`。

### 2. 先做 2 步真实样例

不要直接全量渲染。先取真实相邻两步（约 2–7 秒），完成捕获→编码→组装→在 WPS/Office 放映态验证：

- 进页首段自动播放
- 空格播下一段
- 最后一段后空格翻页
- 无修复提示、无黑块、无可见跳变

通过后再全量。字体错误、native/appear 模式错误、边界问题都会在这个阶段暴露。

### 3. 捕获

```bash
node scripts/capture.mjs --html page.html --out out/capture --width 1920 --height 1080 --fps 60
```

Chrome 无头 + CDP，逐帧 `seek` + 截图，不是实时录屏。捕获器会先做字体预检，并检查 `Runtime.evaluate` 的 `exceptionDetails`；页面脚本抛错时直接失败，不会静默重复帧。

### 4. 编码

```bash
node scripts/encode.mjs --manifest out/capture/manifest.json --out out/clips --crf 14
```

默认 H.264 / yuv420p / 60fps / 无音轨 / `+faststart`；每步一个 MP4 + 首帧 PNG。

**WPS 目标必做**：WPS 12.1 的 native 媒体点击序列对单段时长敏感，超过约 1 秒时可能把“空格”当作翻页。先压到 ≤0.9 秒：

```bash
node scripts/compress-clips.mjs --manifest out/clips/manifest.json --out out/clips-wps --max 0.9
```

每段只保留转场动画，末帧保持；停留时间由演讲者按空格控制。压缩后仍是 60fps，只是时间轴被压缩。然后用 `out/clips-wps/manifest.json` 组装。

### 5. 校验

```bash
python3 scripts/verify.py --capture out/capture/manifest.json --clips out/clips/manifest.json
```

边界比较使用**捕获源 PNG**，不是解码后的 MP4 帧（有损编码不可能像素一致）。同时校验帧数、h264、yuv420p、分辨率、时长。

### 6. 组装 PPTX

```bash
PYTHONPATH=vendor python3 scripts/build_pptx.py \
  --source deck.pptx --clips out/clips/manifest.json \
  --after 12 --mode native --out deck-animated.pptx
```

单页：所有片段铺满整页，第一段在最底层。native 模式利用 python-pptx 自动生成的 `p:video` 时序：首段 `delay=0`，其余 `indefinite`；第 2..N 段使用透明海报帧。appear 模式（后备）注入 `p:seq`，每步 Appear + `playFrom(0.0)`。

详见 `references/pptx-timing.md`。

### 7. 结构校验

```bash
PYTHONPATH=vendor python3 scripts/verify_pptx.py \
  --source deck.pptx --out deck-animated.pptx \
  --new-index 13 --expected-slides 20 --expected-videos 8 --mode native
```

检查：页数、新页位置、原页 C14N 不变、视频数、动画模式门禁。

### 8. 现场兼容性

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/wps_smoke.ps1 -Path <pptx> -Out <png>
```

自动冒烟只证明“能打开、页面首帧正确”，不证明放映态空格分步。预览图应优先与**源首帧**逐像素/低差异比较；深色片头不要用固定 `mean > 5` 阈值，否则会误报。必须在 WPS 和至少一台 Office 上人工按空格验收。WPS 12.1 可能拒绝 `Visible=false`；捕获异常后回退 `Visible=true` 并记录。详见 `references/compatibility.md`。

## 模式 B：实时浏览器启动（WPS 兼容）

当模式 A 在 WPS 上因单段时长失败，或需要完整交互时，优先用自动监视器：

1. 用 `build_auto_deck.py` 生成自动启动页（无按钮）：
   ```bash
   PYTHONPATH=vendor python3 scripts/build_auto_deck.py \
     --source deck.pptx --after 12 --out demo/deck-live.pptx \
     --title '六条设计公理 · 实时演示' \
     --subtitle '切换到本页后，浏览器动画将自动启动'
   ```
2. 同一文件夹放 `deck-live.pptx`、`watch-slides.vbs`、`bring-edge.ps1`、`bring-wps.ps1`、`focus-lib.ps1`、`black-cover.ps1`、`launch-animation.vbs`、`V2.html`。
3. HTML：`<title>TEF-LIVE-STAGE</title>`；Esc 和最后一步后的 Space 调 `window.close()`。
4. 运行 `powershell.exe -File scripts/preflight-live.ps1 -Folder demo` 做 Edge/文件/COM 预检。
5. 目标机聚焦自检：`powershell.exe -ExecutionPolicy Bypass -File scripts/test-focus.ps1`，看到 `PASS` 再进行放映验收（测试会短暂切换焦点并自动还原）。
6. 双击 `watch-slides.vbs`：它启动 WPS、打开 deck、从第 1 页放映；到目标页退出放映，再启动 Edge 并自动聚焦；Edge 关闭后 `Run()+FocusShow+GotoSlide(13)` 回到第 13 页并自动聚焦，用户再按空格到第 14 页。
7. 首次运行监视器可能提示一次；无逐页提示。
8. 无法运行监视器时退回按钮模式：`build_launcher_deck.py` + 相对超链接 `launch-animation.vbs`。如果要在现有内容页上点击启动并返回焦点：launcher 先把 HTML 复制到 `%TEMP%\tef-live-stage\` 的 ASCII 路径，再以独立 `--user-data-dir` 启动 Edge；同步调用 `bring-edge.ps1 -ProfileMarker "<marker>" -StartTimeoutSeconds 20`，Edge 关闭后调用 `bring-wps.ps1` 返回放映焦点。

监视器带 WPS 启动/打开/放映/跳页重试、15 秒心跳单实例锁、Edge 关闭 600 秒超时；卡住双击 `stop-watch.vbs`。

WPS 实测：VBA 宏不可用；WPS 全屏放映无法可靠被普通置顶窗口覆盖；先退出放映再启动 Edge 有效；相对超链接按钮有效；Esc/最后空格 `window.close()` 有效。

详见 `references/live-browser-mode.md`。

### 模式 B2：现有内容页空格触发 + 黑场过渡（不新增页）

适用：现有内容页要嵌入 HTML，不能加页、不能覆盖页面内容，希望按一次空格启动而不是点按钮，并且 PPT/Edge 切换不能露馅。

- 不要用页面形状超链接到 VBS：WPS 每次点击都会弹出文件安全确认，而且多一层 `wscript`/`powershell` 启动。
- 用后台 watcher 轮询 `CurrentShowPosition`：在目标页等待“目标页→下一页”的跳转，触发后启动对应 HTML。
- 黑场：`black-fade.ps1` 先渐黑 0.3s；watcher 等 350ms 确保全黑后再退出 WPS/启动 Edge；Edge 聚焦后淡出 0.3s。最短 1.6s，Edge 慢时自动延长。
- 常驻 agent：`focus-agent.ps1` 在 watcher 启动时预热，按 `agent-arm.flag` 聚焦 Edge，写 ready/exit 标志，避免每次触发都现起 PowerShell。
- 返回：HTML 最后一步空格或 Esc 时把标题改成 `TEF-LIVE-RETURN`，页面内渐黑并延迟 2s 关闭；agent 看到标题后通知 watcher 在 Edge 关闭前起全屏黑场并恢复 WPS，页面关闭后再淡出。
- 模板：`scripts/watch-live-content.vbs`；黑场：`scripts/black-fade.ps1`；聚焦 agent：`scripts/focus-agent.ps1`；停止：`scripts/stop-live.vbs`；完整说明：`references/live-content-trigger.md`。

已知缝隙：触发侧按空格后 WPS 会先翻到下一页，但黑场会盖住；返回侧靠标题信号在 Edge 关闭前起黑场。Alt+F4 等没有标题信号的关闭仍可能看到极短缝隙。

## 关键规则（Red Flags）

- **字体未命中就捕获** → 整批帧作废；先跑 preflight
- **一步一页** → 违反页数限制；必须单页分段
- **实时录屏** → 帧不稳定；必须逐帧 seek
- **WPS 单页视频 >1s** → 点击序列可能失效；压缩到 ≤0.9s（转场 + 末帧保持）
- **把“停留”烘焙进视频** → 视频越长越容易触发 WPS 限制；让演讲者用空格控制停留
- **不检查 exceptionDetails** → `seek` 抛错时静默产出重复帧
- **双重导航** → `loadEventFired` 竞态；先 about:blank 再单次导航
- **用解码 MP4 帧做像素比较** → 有损编码必然失败；比较源 PNG
- **native/appear 混淆** → native 用 `p:video` 节点；appear 才注入 `p:seq`
- **透明海报遗漏** → 第 2..N 段海报会遮住上一段
- **步骤局部动画未关闭** → 边界帧不一致；capture 下关闭 detail/seal keyframes
- **提交生成物** → 视频/帧/成品 PPTX 不入库
- **依赖 WPS VBA 宏** → COM 下 VBComponents 为空；用启动器超链接
- **运行程序动作相对路径** → WPS 不触发；用相对超链接
- **Esc 只退出全屏** → HTML 加 `window.close()`；否则提示 Alt+F4
- **实时浏览器方案漏带文件** → PPTX、VBS、PS1、HTML 必须同目录
- **WPS 全屏放映挡住 Edge** → 先退出放映再启动 Edge，Edge 关闭后回到当前页（第 13 页）
- **Edge 启动后不在最前** → 用 `bring-edge.ps1` 持续置顶；HTML 标题必须是 `TEF-LIVE-STAGE`
- **后台脚本抢不到前台** → Windows 前台锁会拒绝直接 `SetForegroundWindow`（日志 `fg=False`）；用 `focus-lib.ps1` 的 `AttachThreadInput` + 模拟 Alt 键解锁
- **不确定目标机能否自动聚焦** → 先跑 `scripts/test-focus.ps1`，看到 `PASS` 再演示；失败时检查执行策略与安全软件
- **现有内容页要空格触发但不想加按钮** → 监视器检测“目标页→下一页”的跳转后启动，返回时跳回目标页并把该页标记 done；不要用逐页 VBS 超链接（每次都会弹 WPS 安全确认）；模板见 `references/live-content-trigger.md`
- **切换时露出 WPS 编辑器/桌面** → 渐黑完成后（`cover hold`）再退出/启动；返回侧 HTML 必须先设 `TEF-LIVE-RETURN` 标题并延迟关闭，让 watcher 在 Edge 关闭前起黑场
- **黑场淡出太早** → 等 `cover-ready.flag`（Edge 聚焦完成 / WPS 恢复完成）再淡出；Edge 慢时保持到 `MaxHoldMs`
- **watcher 偶发闪退** → WPS COM 瞬时错误（如 err=462）曾被视为致命；新版重试 20 次并记录 `COM busy`，连续失败才 `Cleanup` 退出；检查 `watch-log.txt`，用 `cscript` 复现可获得 VBScript 运行时错误
- **Edge 弹出同步/隐式登录提示** → 启动时加 `--disable-sync`、`--disable-features=msImplicitSignin,msEdgeSync,...` 参数组；仍出现时删除 `%TEMP%\tef-live-*` 或使用 `SyncDisabled`/`ImplicitSignInEnabled` 策略（全局影响，需确认）
- **返回 WPS 后空格无效** → `FocusShow` 先 `SlideShowWindow.Activate`，再用 `bring-wps.ps1` 枚举 `wpp.exe`/`wps.exe` 全屏窗口强制聚焦
- **WPS↔Edge 切换有缝** → 默认流程接受短暂切换；真正无缝需 PowerPoint 或 OBS
- **最后一步空格无效** → 在 `step === TOTAL_STEPS - 1 && stepMs >= stepDuration` 时调 `window.close()`
- **监视器卡住/重复启动** → 双击 `stop-watch.vbs`；陈旧 `watcher.lock` 15 秒后自动清理

## 降级顺序

1. native（默认，兼容性最好）
2. appear（Appear + playFrom）
3. 每步静态 PNG + 原生淡入
4. 整段单视频进页自动播放（放弃分步）

## 文件

- `scripts/` — 预检、捕获、编码、校验、组装、WPS 冒烟
- `scripts/launch-edge-app.vbs` — 模式 B：Edge 全屏启动器（按钮模式）
- `scripts/build_launcher_deck.py` — 模式 B：启动页生成器（按钮模式）
- `scripts/watch-slides.vbs` — 模式 B：自动监视器（推荐）
- `scripts/watch-live-content.vbs` — 模式 B2：现有内容页空格触发 + 黑场监视器模板
- `scripts/focus-agent.ps1` — 模式 B2：常驻 Edge 聚焦 agent（预热、ready/exit/return 信号）
- `scripts/black-fade.ps1` — 模式 B2：全屏黑场（渐黑 / 保持 / 渐透明）
- `scripts/stop-live.vbs` — 模式 B2：结束 watcher、Edge、黑场、agent 与聚焦助手
- `scripts/bring-edge.ps1` — 模式 B：Edge 强制置顶/聚焦，支持 `-ProfileMarker`、`-StartTimeoutSeconds` 与 `-ExitFlag`
- `scripts/bring-wps.ps1` — 模式 B：恢复后找回并聚焦 WPS/PowerPoint 全屏放映窗口
- `scripts/focus-lib.ps1` — 模式 B：Win32 窗口查找/主窗口选择/前台解锁共享库
- `scripts/test-focus.ps1` — 模式 B：前台聚焦自检（会短暂切换焦点并自动还原）
- `scripts/black-cover.ps1` — 模式 B：黑场过渡（实验性，默认不启用；5 秒自动隐藏）
- `scripts/stop-cover.ps1` / `stop-cover.vbs` — 模式 B：黑场应急关闭（实验性）
- `scripts/stop-watch.vbs` — 模式 B：结束监视器与 Edge app 的应急脚本
- `scripts/build_auto_deck.py` — 模式 B：自动启动页生成器
- `scripts/preflight-live.ps1` — 模式 B：Edge/文件/COM 预检
- `references/html-contract.md` — HTML 契约与 capture 模式细则
- `references/pptx-timing.md` — native/appear 时序与 OOXML 结构
- `references/compatibility.md` — WPS/Office 实测与降级
- `references/live-browser-mode.md` — 模式 B：实时浏览器、WPS 实测与部署
- `references/live-content-trigger.md` — 模式 B2：现有内容页空格触发的配置、HTML 契约、验证与排障
- `templates/stepwise-demo.html` — 2 步确定性样例
- `templates/config.example.json` — 配置模板

## 验收清单

- [ ] 字体 preflight 通过
- [ ] HTML 契约预检通过
- [ ] 2 步真实样例在 WPS/Office 放映态通过
- [ ] 边界帧逐像素一致
- [ ] 页数只 +1，原页 C14N 不变
- [ ] 空格步进次数 = 步数，最后一步后翻页
- [ ] 模式 B：点击启动、空格步进、Esc/Alt+F4 返回、返回后翻页
- [ ] 无宏、离线、无修复提示
- [ ] 生成物未入库
