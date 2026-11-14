# 模式 B：实时浏览器启动（WPS / PowerPoint）

当视频分段方案在目标环境不可用，或需要保留完整 HTML 交互时，用“本地启动器 + Edge 全屏应用窗口”运行活的 HTML。

有两种子模式：

| 子模式 | 触发方式 | 安全提示 | 适用 |
|--------|----------|----------|------|
| **自动模式（推荐）** | 后台监视器检测到目标幻灯片后自动启动 Edge | 仅首次运行监视器时可能提示；无逐页提示 | 答辩现场、需要无缝 |
| 按钮模式（后备） | 幻灯片按钮相对超链接到 VBS | 每次点击可能提示 | 无法运行监视器时 |

## 自动模式：watch-slides.vbs

### 机制

```text
双击 watch-slides.vbs
  ├─ 启动 WPS、打开 deck、从第 1 页开始放映
  ├─ FocusShow：SlideShowWindow.Activate + bring-wps.ps1 聚焦放映窗口
  ├─ 轮询 CurrentShowPosition
  ├─ 到达目标页（默认第 13 页）
  │    ├─ 退出 WPS 放映（避开全屏覆盖层）
  │    ├─ 启动 Edge：--app=file:///.../V2.html --start-fullscreen
  │    └─ bring-edge.ps1 持续把 Edge 设为置顶/前台（Win32 前台解锁）
  ├─ 等待 Edge 关闭（HTML 最后一步后按空格 → window.close()）
  ├─ Run() 重新放映；FocusShow 找回 WPS 焦点
  └─ GotoSlide(13) 显式回到目标页；用户再按空格进入第 14 页
```

### 关键发现

WPS 12.1 的放映窗口是特殊的全屏覆盖层：普通窗口即使调用 `SetWindowPos(HWND_TOPMOST)` + `SetForegroundWindow`，也可能被 WPS 覆盖。仅靠置顶不可靠。

可靠做法是：**在目标页先退出 WPS 放映，再启动 Edge**；此时没有全屏覆盖层，Edge 自然在最前。Edge 关闭后，监视器 `Run()` 并显式 `GotoSlide(13)` 回到第 13 页（WPS 可能忽略 `StartingSlide`），用户再按空格进入第 14 页。

这不是宏方案，也不需要 VBA：WPS 的 COM `VBProject.VBComponents` 是空壳，无法注入宏。

### Windows 前台锁

第二个容易踩的坑是自动聚焦：`watch-slides.vbs` 由后台 `wscript.exe` 运行，`bring-edge.ps1` 又是隐藏 `powershell.exe`，直接 `SetForegroundWindow` 会被 Windows 前台锁拒绝（现象：窗口已置顶，`bring-log.txt` 却是 `fg=False`，键盘输入仍发往旧窗口）。

修复方式统一放在 `focus-lib.ps1` 的 `TefWindow.Force`：

1. 先普通 `SetForegroundWindow`。
2. 失败后 `SwitchToThisWindow`。
3. 仍失败则 `AttachThreadInput` 到当前前台线程和目标窗口线程，`BringWindowToTop` + `SetForegroundWindow` + `SetFocus`。
4. 最后模拟 Alt 键按下/抬起解锁前台锁，并校验 `GetForegroundWindow`。

Edge 方向由 `bring-edge.ps1` 持续校验到窗口关闭；WPS 方向由 `watch-slides.vbs` 的 `FocusShow()` 先调 `SlideShowWindow.Activate`，再调 `bring-wps.ps1`。WPS 不公开 `SlideShowWindow.HWND`，`bring-wps.ps1` 通过枚举 `wpp.exe`/`wps.exe` 的可见顶层窗口，按全屏尺寸、类名（`Widget`/`screenClass`/`SlideShowClass`）、标题（含“幻灯片”/`Slide Show`）、`WS_POPUP`、无最小/最大化按钮等特征打分来定位放映窗口。

Edge 启动统一附带 `edgeFlags`（`--disable-sync`、`--disable-features=msImplicitSignin,msEdgeSync,...`、`--no-first-run` 等），用于抑制同步/隐式登录弹窗。该参数组已实测有效；官方策略兜底（`SyncDisabled`/`ImplicitSignInEnabled`）见 [live-content-trigger.md](live-content-trigger.md)。

黑场过渡为实验性功能，默认不启用；实际使用中出现过卡黑屏，因此默认流程只做退出/恢复，切换时可能短暂看到 WPS 编辑界面。真正无缝需 PowerPoint 或 OBS。

### 抗逆性

- 启动预检：deck、V2.html、Edge 缺失时给出明确 MsgBox 并退出。
- WPS 启动、打开 deck、开始放映、GotoSlide 均 3 次重试；每次重试有间隔。
- 单实例心跳锁：每 5 秒刷新 `watcher.lock`；15 秒内视为运行中，陈旧锁自动删除。
- Edge 关闭检测 600 秒超时；超时强制恢复 WPS 第 13 页。
- 恢复后等待 1.5 秒再判断当前页，避免 `CurrentShowPosition` 延迟导致误判。
- 初始放映与恢复放映都调用 `FocusShow()`；`bring-wps.ps1` 最多重试 6 秒。
- Edge 方向 `bring-edge.ps1` 每 300ms 校验前台，聚焦失败每 5 秒记一次日志。
- `test-focus.ps1` 在目标机做前台聚焦自检：创建独立测试窗口，验证 `TefWindow.Force` 与焦点还原；约 1–2 秒，看到 `PASS` 再进行放映。
- `stop-watch.vbs`：结束监视器、聚焦助手（bring-edge/bring-wps）与 Edge app，清理锁与标志；应急用。
- 日志带时间戳，每次写日志打开/关闭文件。

### 文件

```text
demo/
  deck-live.pptx
  watch-slides.vbs      # 自动监视器
  bring-edge.ps1        # 持续强制置顶/聚焦 Edge
  bring-wps.ps1         # 恢复放映后聚焦 WPS 放映窗口
  focus-lib.ps1         # Win32 窗口查找/前台解锁共享库
  test-focus.ps1        # 可选：前台聚焦自检（短暂切换焦点）
  black-cover.ps1       # 黑场过渡（实验性，默认不启用）
  launch-animation.vbs  # 手动后备
  V2.html
```

- `watch-slides.vbs` 默认打开同目录的 `LyMtrix-TEF-Lv6-live.pptx`、目标页 13、加载 `V2.html`。换文件名/页码时修改脚本顶部的 `deck` 和 `targetSlide`。
- `bring-edge.ps1`、`bring-wps.ps1`、`focus-lib.ps1` 必须同目录；监视器会以隐藏 PowerShell 调用前两个，二者再点源 `focus-lib.ps1`。
- `black-cover.ps1` 为实验性文件，默认监视器不启动它。
- PPTX、VBS、PS1、HTML 必须一起分发。

### 自动页生成与预检

生成无按钮的自动启动页：

```bash
PYTHONPATH=vendor python3 scripts/build_auto_deck.py \
  --source deck.pptx --after 12 --out demo/deck-live.pptx \
  --title '六条设计公理 · 实时演示' \
  --subtitle '切换到本页后，浏览器动画将自动启动'
```

预检（Edge、文件、WPS COM 注册）：

```powershell
powershell.exe -File scripts/preflight-live.ps1 -Folder demo
```

### HTML 要求

1. `<title>TEF-LIVE-STAGE</title>`：置顶助手靠这个唯一标题找到 Edge 窗口。
2. 交互模式 keydown：
   - `Escape` → `window.close()`
   - 最后一步播完后 `Space` → `window.close()`；否则调用 `advanceStep()`。

```js
if (e.code === 'Space') {
  e.preventDefault();
  if (step === TOTAL_STEPS - 1 && stepMs >= stepDuration(step)) {
    window.close();
    return;
  }
  advanceStep();
} else if (e.key === 'Escape') {
  e.preventDefault();
  window.close();
}
```

### 使用

1. 关闭已经打开的 deck 副本。
2. 双击 `watch-slides.vbs`（不要先手动打开 PPTX）。
3. 它会启动 WPS、打开 deck，并从第 1 页开始放映。
4. 翻到目标页时，WPS 退出放映，Edge 全屏打开并自动聚焦（可能短暂看到 WPS 编辑界面）。
5. 空格逐步推进；最后一步后按空格关闭 Edge。
6. WPS 回到第 13 页并自动聚焦；再按空格进入第 14 页。

### 测试钩子

`watch-slides.vbs /test` 会在启动后自动跳到目标页，用于自动化验收；正式演示不要带 `/test`。

目标机可以先单独验证前台聚焦，不会启动 WPS/Edge：

```powershell
powershell.exe -ExecutionPolicy Bypass -File scripts/test-focus.ps1
```

输出 `PASS: foreground activation works` 表示 `AttachThreadInput`/Alt 解锁可用；输出 `FAIL` 时检查执行策略、安全软件或组策略。

### 日志

- `watch-log.txt`：启动、到目标页、退出放映、Edge 关闭、回到第 13 页。
- `bring-log.txt`：聚焦助手找到的窗口句柄、类名、标题，以及 `focused` / `focus failed` 结果。
- 以上日志/标志都可删除。
- 黑场为实验性功能；如误开，双击 `stop-cover.vbs` 立即关闭。
- 监视器每次写日志都打开/关闭文件，避免第二个实例占用日志导致 Windows Script Host 800A0046。
- `watcher.lock` 防止重复双击；提示 `already running` 时删除该文件（10 分钟后自动视为过期）。

## 按钮模式（后备）

PPTX 第 N 页放一个可见按钮，使用**相对超链接**指向 `launch-animation.vbs`：

```text
按钮（相对超链接）→ launch-animation.vbs
  → msedge.exe --app=file:///.../V2.html --start-fullscreen
```

- 按钮必须可见；透明整页形状在 WPS 中不一定接收点击。
- WPS 实测：相对超链接有效；相对“运行程序”动作无效；绝对路径有效但不可移植。
- 首次点击会出现“是否打开/运行”的安全提示，选择允许。
- 可以用 `scripts/build_launcher_deck.py` 生成按钮页；页数上限紧张时替换现有静态页。

### 按钮模式：点击启动并返回焦点

如果在**现有内容页**上加按钮（不新增页、不覆盖原内容），按钮只需一个相对超链接指向 launcher VBS。推荐 launcher 使用以下模式：

1. 把 HTML 复制到 `%TEMP%\tef-live-stage\` 下的 ASCII 文件名，再用 `file:///C:/...` 启动。这样可避开 UNC 路径和中文目录导致的 Edge `--app` 加载失败。
2. 每个 HTML 使用独立的 `--user-data-dir` 标记，例如 `tef-live-audit`。
3. 同步调用 `bring-edge.ps1 -ProfileMarker "<marker>" -StartTimeoutSeconds 20`；它会按配置目录定位 Edge 窗口，选择面积最大的可见窗口（避开启动瞬间的小 splash 窗口），持续聚焦到窗口关闭。
4. Edge 关闭后调用 `bring-wps.ps1 -TimeoutSeconds 6`，把焦点还给仍在放映的 WPS 或 PowerPoint。

按钮形状必须可见（透明整页形状在 WPS 中不一定接收点击）；放在页脚或空白区，避免覆盖原有内容。命令示例：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File bring-edge.ps1 -ProfileMarker "tef-live-audit" -StartTimeoutSeconds 20
powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File bring-wps.ps1 -TimeoutSeconds 6
```

### 现有内容页：空格触发（不新增页）

如果不想加按钮、又要求“按一次空格才启动”，不要用超链接（每次点击都会触发 WPS 安全确认）。改用监视器检测放映位置：

1. 监视器记录当前页 `prevSlide`；当检测到 `pos > prevSlide` 且 `prevSlide` 是目标页时触发。
2. 触发后把该目标页标记为 done，再启动 Edge；Edge 关闭后恢复放映并跳回目标页。
3. done 标记保证返回后再次按空格只翻页，不会重复启动同一页。
4. HTML 在最后一步的 Space 或 Esc 调 `window.close()`；监视器用 `bring-edge.ps1` 的 `-ExitFlag` 判断 Edge 窗口已关闭。
5. 监视器只在启动时请求一次安全允许，没有逐页确认。

注意：WPS 会先执行空格翻到下一页。新版模板用 `black-fade.ps1` 先渐黑，等全黑后再退出放映/启动 Edge，并在 Edge 就绪后淡出；返回侧 HTML 用 `TEF-LIVE-RETURN` 标题提前通知 watcher，在 Edge 关闭前起黑场并恢复 WPS。实现可参考 `docs/competition/完整最终版/初赛短版本/watch-live.vbs`；通用模板为 `scripts/watch-live-content.vbs`，完整配置、黑场/返回信号、HTML 契约与验收步骤见 [live-content-trigger.md](live-content-trigger.md)。

## WPS 12.1 实测结论

| 机制 | 结果 |
|------|------|
| VBA 宏注入（COM `VBProject.VBComponents`） | 失败：VBComponents 为 null |
| 可见按钮 + 相对超链接到 VBS | **有效**，有安全提示 |
| 可见按钮 + 相对运行程序动作 | 无效 |
| 后台监视器 + 直接启动 Edge | 有效，但 Edge 可能被 WPS 全屏覆盖 |
| 监视器：先退出放映再启动 Edge | **有效**，Edge 在最前 |
| Edge 关闭后监视器回到第 13 页 | **有效** |
| HTML `window.close()`（Esc / 最后空格） | 有效，关闭 Edge app 窗口 |
| Alt+F4 | 有效后备 |
| 后台脚本直接 `SetForegroundWindow` | 失败，前台锁拒绝（日志 `fg=False`） |
| `AttachThreadInput` + 模拟 Alt 键 | 修复后台脚本聚焦 |
| `SlideShowWindow.Activate` + 枚举 WPS 全屏窗口 | 修复返回 WPS 后的焦点 |

## 限制与风险

- 不是单文件方案；HTML/VBS/PS1 必须随 deck 分发。
- 自动模式会短暂退出 WPS 放映，再启动 Edge；Edge 关闭后回到第 13 页，用户再按空格进入第 14 页。视觉上由 Edge 全屏接管。
- 首次运行监视器可能出现安全提示；无逐页提示。
- Edge 必须存在且未被策略禁用；启动器会依次尝试 x86/x64/`msedge.exe`。
- `--app` 窗口可能有极细标题栏；`--start-fullscreen` 或 F11 处理。
- Windows 前台锁可能阻止后台脚本自动聚焦；已用 `focus-lib.ps1` 的 `AttachThreadInput` + 模拟 Alt 键处理。若目标机策略更严格，检查 `bring-log.txt` 并用鼠标点击一次作为后备。
- 若现场完全禁止运行本地脚本，退回视频模式或静态图模式。
- 监视器以只读方式打开 deck；需要编辑请直接打开 PPTX。
