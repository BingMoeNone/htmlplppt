# 模式 B2：现有内容页空格触发 + 黑场过渡（不新增页）

> **适用**：必须在现有内容页上嵌入 HTML；不能新增页、不能覆盖页面内容；希望按一次空格启动 HTML；PPT 与 Edge 之间需要无缝黑场过渡；现场是 WPS/PowerPoint + Edge。
> **已验证实现**：`docs/competition/完整最终版/初赛短版本/watch-live.vbs`（2026-11-13，WPS 12.1 + Edge）
> **模板**：`scripts/watch-live-content.vbs`；**黑场**：`scripts/black-fade.ps1`；**聚焦 agent**：`scripts/focus-agent.ps1`；**停止**：`scripts/stop-live.vbs`
> **相关**：[live-browser-mode.md](live-browser-mode.md)、[compatibility.md](compatibility.md)

## 1. 为什么不使用按钮超链接

在 WPS 中，页面形状指向 `.vbs` 的相对超链接每次点击都会触发文件安全确认；同时还要多启动一层 `wscript`/`powershell`。用户要求“按空格即可”时，不要用按钮 + 超链接，改用后台监视器直接启动 Edge：

- 只在一开始双击 `watch-live-content.vbs` 时可能需要允许一次；
- 之后每页触发都没有确认框；
- Edge 由 watcher 直接启动，少一层 launcher。

## 2. 架构

```text
双击 watch-live-content.vbs
  ├─ 启动 black-fade.ps1（常驻黑场，预热）
  ├─ 启动 focus-agent.ps1（常驻聚焦 agent，预热）
  ├─ 启动 WPS、打开 deck、从第 1 页放映、聚焦
  ├─ 轮询 CurrentShowPosition
  ├─ 在目标页等待空格（WPS 翻到下一页）
  ├─ 检测到“目标页 -> 下一页”
  │    ├─ 写 cover-fade.flag（渐黑 0.3s）
  │    ├─ 等待 350ms（确保屏幕全黑）
  │    ├─ 写 agent-arm.flag + 启动 Edge
  │    ├─ agent 找到并聚焦 Edge -> 写 cover-ready.flag
  │    ├─ 退出 WPS 放映
  │    └─ cover 等 cover-ready 且黑屏 >=1s -> 渐透明 0.3s
  ├─ HTML 内空格推进；最后一步空格或 Esc
  │    ├─ HTML 标题改为 TEF-LIVE-RETURN，页面内渐黑，2s 后 window.close()
  │    ├─ agent 轮询标题 -> 写 return-request.flag
  │    ├─ watcher 写 cover-fade.flag，等 350ms
  │    ├─ ResumeShow（WPS 在 Edge 仍打开时恢复，黑场覆盖）
  │    └─ 页面关闭 -> cover-ready -> 渐透明到目标页
  └─ 继续按空格进入下一页；同一目标页不再触发
```

## 3. 文件

演示文件夹：

```text
demo/
  deck.pptx                 # 原稿，不修改、不新增页
  watch-live-content.vbs    # 监视器（空格触发 + 黑场 + 返回信号）
  stop-live.vbs             # 应急停止
  focus-agent.ps1           # 常驻 Edge 聚焦 agent（预热，处理 ready/exit/return）
  black-fade.ps1            # 全屏黑场：渐黑 / 保持 / 渐透明
  bring-wps.ps1             # 返回时聚焦 WPS/PowerPoint 放映窗口
  focus-lib.ps1             # Win32 窗口查找/前台解锁共享库
  page1.html                # 目标页 1 的 HTML
  page2.html                # 目标页 2 的 HTML
  page3.html                # 目标页 3 的 HTML
```

所有文件必须同目录；不要单独移动 PPTX。非 ASCII 文件名（如 `审计.html`）在 VBS 中用 `ChrW` 构造，见第 4 节。

## 4. 配置

`scripts/watch-live-content.vbs` 顶部有 CONFIG 区：

| 变量 | 含义 |
|------|------|
| `deckName` | PPTX 文件名（同目录）；非 ASCII 用 `ChrW` |
| `targetSlides` | 触发页号数组，例如 `Array(5, 6, 8)` |
| `srcNames` | 源 HTML 文件名数组（同目录） |
| `targetHtmls` | 复制到 `%TEMP%\tef-live-stage` 的 ASCII 文件名数组 |
| `targetProfiles` | 每个 HTML 独立的 Edge `--user-data-dir` 标记，建议 `tef-live-` 前缀 |
| `targetLabels` | 日志标签 |

示例（含中文文件名）：

```vbs
deckName = "LyMtrix-TEF - " & ChrW(&H5B8C) & ChrW(&H5168) & ChrW(&H7248) & ".pptx"
targetSlides = Array(5, 6, 8)
srcNames = Array("TrustedZones-LV2.html", "ContractEngine-LV2.html", ChrW(&H5BA1) & ChrW(&H8BA1) & ".html")
targetHtmls = Array("TrustedZones-LV2.html", "ContractEngine-LV2.html", "audit.html")
targetProfiles = Array("tef-live-trustedzones", "tef-live-contractengine", "tef-live-audit")
targetLabels = Array("TrustedZones", "ContractEngine", "Audit")
```

规则：

- 所有数组长度必须一致；脚本启动时会校验。
- `targetHtmls` 使用 ASCII 名称，避免 UNC/中文路径导致 Edge `--app` 打开空白页。
- watcher 启动时会把 `srcNames(i)` 复制到 `%TEMP%\tef-live-stage\targetHtmls(i)`。
- 每个 HTML 使用独立 profile，避免多个页面复用同一个 Edge 实例产生窗口歧义。

## 5. HTML 要求

1. 页面为自包含 HTML；如需外部资源，必须与 HTML 同目录或在 HTML 内联。
2. 空格推进步骤，同时接受两种事件写法：

   ```js
   if (e.code === "Space" || e.key === " ") { e.preventDefault(); nextStep(); }
   ```

   只判断 `e.code === "Space"` 时，某些合成按键/输入法路径可能收不到；同时判断 `e.key` 更稳。

3. 最后一步空格或 Esc 进入**返回黑场**，不要直接 `window.close()`：

   ```js
   window.__tefReturn = function () {
     if (window.__tefReturning) return;
     window.__tefReturning = true;
     document.title = 'TEF-LIVE-RETURN';   // 给 agent 的信号
     var cover = document.createElement('div');
     cover.style.cssText = 'position:fixed;left:0;top:0;right:0;bottom:0;background:#000;z-index:2147483647;opacity:0;transition:opacity 0.3s linear;pointer-events:none';
     document.body.appendChild(cover);
     requestAnimationFrame(function () { cover.style.opacity = '1'; });
     setTimeout(function () { window.close(); }, 2000);
   };
   window.addEventListener('keydown', function (e) {
     if (e.key === 'Escape' || e.key === 'Esc') {
       e.preventDefault(); e.stopPropagation(); window.__tefReturn();
     }
   }, true);
   ```

   - 页面内先渐黑，避免 Edge 窗口在关闭前露出下一页。
   - `TEF-LIVE-RETURN` 标题让 `focus-agent.ps1` 在 Edge 关闭前通知 watcher。
   - 2 秒延迟是给 WPS 恢复留时间；可按现场 ResumeShow 耗时调整。

4. 不要要求页面先被鼠标点击才接收键盘；Edge app 窗口打开后应直接响应空格。若页面依赖 `document.activeElement`，在启动时调用 `window.focus()`。

## 6. 黑场与返回信号

### 黑场 `black-fade.ps1`

- 全屏、置顶、无激活、点击穿透；覆盖虚拟屏幕（多显示器）。
- 时间参数：`FadeMs=300`、`HoldMs=1000`、`MaxHoldMs=3000`。
- 最短序列：渐黑 0.3s + 保持 1.0s + 渐透明 0.3s = 1.6s。
- watcher 在写 `cover-fade.flag` 后等待 350ms，确保屏幕完全黑，再执行 WPS 退出/Edge 启动。
- 黑场保持到 `cover-ready.flag` 出现（或 3s 上限）才渐透明；Edge 启动慢时黑屏会自动延长，避免露出桌面。
- 信号文件：`cover-fade.flag`、`cover-ready.flag`、`cover-done.flag`、`cover-stop.flag`、`cover-hide.flag`。

### 常驻聚焦 agent `focus-agent.ps1`

- watcher 启动时预热，避免每次触发现起 PowerShell/Add-Type。
- 监听 `agent-arm.flag`，格式：`<profile>|<exitFlag>|<readyFlag>`。
- 找到 Edge 主窗口后聚焦；首次聚焦写 `readyFlag`（黑场据此淡出）；窗口关闭写 `exitFlag`。
- 轮询 Edge 窗口标题；发现 `TEF-LIVE-RETURN` 时写 `return-request.flag`，让 watcher 在 Edge 关闭前开始返回黑场。
- 心跳：`watcher.lock` 缺失或超过 30s、或 `agent-stop.flag` 出现时退出。

### 返回侧时序

1. 用户按 Esc/最后一步空格：页面标题改为 `TEF-LIVE-RETURN`，页面内渐黑，2s 后关闭。
2. agent 在 ~100ms 内看到标题，写 `return-request.flag`。
3. watcher 收到后写 `cover-fade.flag`，等 350ms，执行 `ResumeShow`（WPS 在 Edge 仍打开时恢复），保持黑场。
4. 页面 2s 后 `window.close()`；watcher 看到 `exitFlag` 后写 `cover-ready.flag`，黑场渐透明，露出已恢复的目标页。

这样返回侧不会先露出 WPS 编辑器/桌面；代价是返回黑场通常比 1.6s 长（约 2–3s，取决于 WPS 恢复和页面 2s 延迟）。如果用户 Alt+F4 关闭 Edge，页面信号不会发出，watcher 走“关闭后起黑场”的兜底路径，可能看到极短缝隙。

### Edge 同步/首启弹窗

短版 watcher 启动 Edge 时统一附带以下参数（模板中为 `edgeFlags`）：

```text
--no-first-run --no-default-browser-check --disable-sync
--disable-features=msImplicitSignin,msEdgeSync,msFirstRunExperience,msEdgeFirstRunExperience,EdgeSync
--disable-background-networking --disable-component-update --disable-extensions
--disable-default-apps --no-service-autorun
--disable-client-side-phishing-detection --disable-domain-reliability
--no-pings --disable-breakpad
```

2026-11-13 实测：同一个全新 `--user-data-dir`，不加参数组会出现“我们正在你的所有设备上同步你的浏览数据 / 已在此设备上登录，因此也使你登录到了 Microsoft Edge”弹窗；加上参数组后不再出现，HTML 正常显示。

注意：这些是 Chromium/Edge 启动参数，不是微软文档承诺的稳定接口；不同 Edge 版本可能忽略其中部分。

如果目标机仍出现同步弹窗，使用 Edge 官方策略兜底（会作用于该用户/机器的所有 Edge 配置，属于全局设置）：

```reg
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\Edge]
"SyncDisabled"=dword:00000001
"ImplicitSignInEnabled"=dword:00000000
"HideFirstRunExperience"=dword:00000001
```

- `SyncDisabled=1`：禁用同步，并抑制同步同意提示。
- `ImplicitSignInEnabled=0`：禁用 Windows 账户隐式登录 Edge，直接对应本次弹窗的“已在此设备上登录”。
- `HideFirstRunExperience=1`：隐藏首启体验和闪屏。
- 需要管理员权限写入 HKLM；可用 `edge://policy` 验证。
- 撤销：删除对应的值或整个 `HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\Edge` 键，重启 Edge。
- 该策略会让 Edge 显示“由组织管理”提示，并影响用户日常 Edge 的同步/登录行为；不要在未确认的情况下自动写入。

如果只是演示 profile 已经登录过，可以删除 `%TEMP%\tef-live-*` 后重新启动 watcher，让新参数组在全新 profile 上生效。

参考：
- [SyncDisabled](https://learn.microsoft.com/en-us/deployedge/microsoft-edge-policies/syncdisabled)
- [HideFirstRunExperience](https://learn.microsoft.com/en-us/deployedge/microsoft-edge-policies/hidefirstrunexperience)

## 7. 触发与返回细节

- watcher 记录 `prevSlide`。当检测到 `pos > prevSlide` 且 `prevSlide` 在 `targetSlides` 中时触发。
- 触发后先把该目标页标记为 done，再启动 Edge；因此返回后再次按空格只会翻页，不会重复启动同一 HTML。
- 返回时 `StartShow` + `FocusShow` + `GotoSlide(target)`；`ignoreUntil` 约 2 秒内忽略位置变化，避免把恢复过程误判为用户翻页。
- 同一场放映每个目标页只触发一次；结束放映后 done 标记重置，重新开始放映可再次触发。

## 8. 已知切换缝

触发发生在“离开目标页”时：按空格后 WPS 会先翻到下一页，watcher 随后在黑场下退出放映并打开 Edge。黑场会盖住下一页/编辑界面，因此正常观看看不到切换缝。

返回侧通过 `TEF-LIVE-RETURN` 标题信号在 Edge 关闭前起黑场，已被验证不会露出 WPS 编辑器；Alt+F4 等无信号关闭仍可能看到极短缝隙。

## 9. 使用步骤

1. 关闭已经打开的 deck 副本。
2. 双击 `watch-live-content.vbs`；首次如提示安全警告，选择“允许/打开”。
3. 正常翻页；到目标页时不会自动跳转。
4. 在目标页按一次空格；黑场渐入，全黑后切换，Edge 就绪后渐出。
5. HTML 内按空格推进；最后一步空格或 Esc 进入返回黑场，返回目标页。
6. 返回后继续按空格进入下一页。
7. 应急：双击 `stop-live.vbs`。

## 10. 测试与日志

自动化验收：

```powershell
wscript.exe watch-live-content.vbs /test
```

`/test` 会在启动后跳到 `targetSlides(0)`，用于快速验证第一个目标页。正式演示不要带 `/test`。

现场检查：

- `watch-log.txt`：`cover started`、`agent started`、`trigger slide N -> Label`、`return requested by page`、`resumed at slide N`、`edge closed after return; cover fade out`。
- `bring-log.txt`：`agent start`、`agent session profile=...`、`agent found`、`agent focused`、`agent return requested`、`agent session end`。
- `cover-log.txt`：`fade in`、`hold`、`fade out held=... ready=True`、`done`。
- 关键时序：`cover hold` 必须早于 `agent session`/`edge launched`；`return requested by page` 必须早于 `agent session end`；`bring-wps focused` 应早于 `agent session end`。
- 可选：`powershell -ExecutionPolicy Bypass -File test-focus.ps1`，看到 `PASS` 表示前台聚焦可用。

### 闪退排查与自清理

已定位的主要风险：watcher 每 100ms 读 `SlideShowWindows.Count`；WPS 在切换/退出/恢复时可能返回瞬时 COM 错误（实测 `err=462`，远程过程调用不可用）。旧版把任何 COM 错误当成“WPS 已关闭”直接退出，表现为概率性闪退。

现在的行为：

- COM 错误先重试 20 次（约 2 秒），日志写 `COM busy: err=<code> <desc> count=<n>`；恢复后继续。
- 连续 20 次失败才判定 WPS 不可用，执行 `Cleanup` 后退出。
- `Cleanup`：
  - 写 `agent-stop.flag` / `cover-stop.flag`；
  - 结束 `tef-live-*` Edge 进程；
  - 结束 `focus-agent.ps1` / `black-fade.ps1` / `bring-wps.ps1` / `bring-edge.ps1`；
  - 删除 `watcher.lock` 与所有 cover/agent/return 标志。
- `black-fade.ps1` 与 `focus-agent.ps1` 都检查 `watcher.lock` 心跳（30 秒）；watcher 崩溃后它们会自动退出。
- 正常的 `Fail`（缺文件、WPS 启动失败、deck 打不开）也走 `Cleanup`，不会留下 Edge/黑场/agent。
- 只有**持锁实例**才执行 `Cleanup`。第二个 watcher 在启动预检失败时不会清理，避免误杀正在运行的第一个实例；不要在 watcher 运行时手动删除 `watcher.lock`。

如果仍然闪退，按顺序检查：

1. `watch-log.txt` 最后是否有 `COM busy` / `cleanup`；错误码是什么。
2. 是否 WPS 本身被关闭/崩溃（`wpp.exe` 不在）——这是预期退出。
3. 是否有第二个 watcher（`watcher.lock` 15 秒内视为运行中）；不要在运行时手动删锁。
4. 启动阶段是否 `Deck not found` / `HTML not found` / `Could not start WPS`。
5. 用 `cscript //nologo watch-live-content.vbs /test` 运行；若有 VBScript 运行时错误会直接打印。
6. `bring-log.txt` / `cover-log.txt` 是否显示 helper 先崩溃；helper 崩溃不会带走 watcher，但会失去聚焦/黑场。

复现自清理：触发 Edge 后强制结束 `wpp.exe`，应看到约 2 秒的 `COM busy` 重试，然后 `cleanup start/done`，最后没有 watcher、Edge、agent、黑场或锁残留。

## 11. 验收清单

- [ ] PPTX 页数与处理前一致，未新增页。
- [ ] 原页面形状、文本、动画 XML 未变化。
- [ ] 第 N 页进入时不自动跳转；按一次空格才启动。
- [ ] 渐黑完成后（`cover hold`）才开始退出 WPS/启动 Edge。
- [ ] Edge 就绪后才渐透明；Edge 慢时黑场自动延长。
- [ ] HTML 内空格推进；最后一步空格或 Esc 进入返回黑场。
- [ ] 返回黑场在 Edge 关闭前启动，WPS 在 Edge 仍打开时恢复，无编辑器/桌面露出。
- [ ] 返回后自动聚焦当前放映页；再按空格不重复触发。
- [ ] 没有逐页安全确认（只有首次启动 watcher 的一次）。
- [ ] `stop-live.vbs` 能结束 watcher、Edge、黑场、agent 与聚焦助手。

## 12. 排障

| 现象 | 处理 |
|------|------|
| `FAIL: Could not start WPS (Kwpp.Application)` | 关闭残留 WPS 演示进程/旧 watcher 后重试；WPS COM 实例可能被挂起 |
| `FAIL: Deck not found` | 检查 `deckName`；中文名用 `ChrW`，不要直接写中文 |
| Edge 打开空白页/新标签页 | 确认 `targetHtmls` 是 ASCII 本地暂存名；不要直接用 UNC/中文路径做 `--app` |
| Edge 弹出同步/隐式登录提示 | 已加入 `edgeFlags` 参数组；仍出现时删除 `%TEMP%\tef-live-*` 重新建 profile，或使用 `SyncDisabled` / `ImplicitSignInEnabled` 策略 |
| 空格不推进 | HTML 同时判断 `e.code === "Space"` 和 `e.key === " "`；确认页面启动时已 `window.focus()` |
| 黑场不出现 | 检查 `black-fade.ps1` 是否随 watcher 启动；查看 `cover-log.txt`；确认 `cover-fade.flag` 被写入 |
| 切换时仍露馅 | 确认 `coverFadeWaitMs` 默认 350ms，且 `cover hold` 早于 `edge launched`；Edge 未被提前启动 |
| 返回时露 WPS 编辑器 | 检查 HTML 是否设置 `TEF-LIVE-RETURN` 标题；agent 是否写 `return-request.flag`；watcher 是否在 `edge closed` 前执行 `ResumeShow` |
| 黑屏过长 | Edge 启动慢；agent 已预热但 Edge 本身可能慢；可调 `MaxHoldMs` 或页面 2s 延迟，但不要早于 WPS 就绪淡出 |
| 同一页重复触发 | 检查 `doneFlags` / `prevSlide` 逻辑；返回后的目标页必须已标记 done |
| 多个 watcher 同时运行 | 检查 `watcher.lock`；不要在 watcher 运行时手动删除锁文件 |
| watcher 闪退 | 看 `watch-log.txt` 的 `COM busy` 错误码；连续 20 次失败会 `Cleanup` 后退出；用 `cscript` 复现可看到 VBScript 错误 |
| WPS 崩溃后残留进程 | 新版会自动 `Cleanup`；若 watcher 被强杀，双击 `stop-live.vbs`，黑场/agent 也会在 30 秒心跳超时后退出 |
| watcher 卡住 | 双击 `stop-live.vbs`；检查 `watch-log.txt` / `bring-log.txt` / `cover-log.txt` |

## 13. 相关文档

- [live-browser-mode.md](live-browser-mode.md) — 模式 B 总览、自动监视器与按钮模式
- [compatibility.md](compatibility.md) — WPS/Office 兼容性与降级
- `scripts/watch-live-content.vbs` — 空格触发 + 黑场 + 返回信号通用模板
- `scripts/black-fade.ps1` — 全屏黑场
- `scripts/focus-agent.ps1` — 常驻聚焦 agent
- `scripts/stop-live.vbs` — 应急停止
- `docs/competition/完整最终版/初赛短版本/watch-live.vbs` — 已验证实现
