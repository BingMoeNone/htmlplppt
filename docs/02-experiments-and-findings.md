# 实验与发现

> 状态：实测记录
> 日期：2026-11-14
> 环境：Windows 10/11 + WPS Office 12.1.0.29690 + Microsoft Edge；Linux/WSL 侧用于捕获与编码
> 关联：[研究议程](00-research-agenda.md)、[架构](01-architecture.md)

## 1. 环境与可复现性

| 项 | 值 |
|----|----|
| WPS | 12.1.0.29690（`wpp.exe` / `KWPP.Application`） |
| Edge | Chromium 内核，`--app` + `--start-fullscreen` |
| 屏幕虚拟化 | PowerShell 进程报告 1755×1097（DPI 缩放后） |
| 捕获 | Chrome 无头 + CDP，`seek` + 截图 |
| 编码 | H.264 / yuv420p / 60fps / 无音轨 |
| 测试 deck | 20 页 live 稿；短版 9 页稿 |

> 注意：WPS/Edge 版本差异会改变结论；本文件记录的是上述版本的实测，不作为跨版本承诺。

## 2. E1：WPS 单页视频时长

| 单页视频 | WPS 放映态结果 |
|----------|----------------|
| 2 段 × 0.816s | 正常逐段播放 |
| 2 段 × 1.816s / 2.45s | 空格被当作翻页，点击序列失效 |
| 8 段 × 0.45–0.83s | 正常 8 步逐段播放 |

**结论**：WPS 12.1 的 native 媒体点击序列对单段时长敏感。面向 WPS 时，每段只保留转场并压到 ≤0.9s；停留时间由演讲者按空格控制。

**含义**：不要把“等待”烘焙进视频；视频是动画片段，不是完整时间轴。

## 3. E2：页数与内容保真

- 源 deck 20 页；模式 A 只新增第 13 页；预期总页数 20。
- `verify_pptx.py` 校验：新页位置、视频数、动画模式门禁、原 19 页 C14N 不变。
- 短版模式 B2：9 页 → 9 页；原页面形状/文本/动画 XML 逐项 C14N 比较一致；只增加外部 watcher，不修改 deck。
- 早期按钮方案：9 页 → 9 页，原形状不变；按钮放在页脚空白区。但逐页超链接会触发 WPS 安全确认，最终被 watcher 方案替代。

**结论**：页面内容保真可以通过“只加形状/只改外部脚本”实现，但交互方式会影响现场体验。

## 4. E3：捕获方式

- **实时录屏**：帧率受渲染抖动影响，边界帧不可重复，难以做像素级验收。
- **CDP 逐帧 seek + 截图**：时间轴由 `window.__tefSteps.seek(step, tMs)` 决定，截图同步；页面脚本抛错时检查 `Runtime.evaluate` 的 `exceptionDetails`，避免静默重复帧。
- **边界比较**：使用捕获阶段的源 PNG，而不是解码后的 MP4 帧；有损编码不可能像素一致。

**结论**：确定性捕获是模式 A 的基础；实时录屏只适合“不要求分步/不要求边界一致”的场景。

## 5. E4：native vs appear

- **native**：python-pptx 生成 `p:video` 时序；首段 `delay=0`，其余 `indefinite`；第 2..N 段使用透明海报帧。兼容性最好，默认使用。
- **appear**：注入 `p:seq`，每步 Appear + `playFrom(0.0)`；作为后备，用于 native 在目标版本失败时。

**结论**：先 native，现场失败再 appear；不要用同一种模式假设所有版本。

## 6. E5：WPS 全屏覆盖层

| 机制 | 结果 |
|------|------|
| 监视器直接启动 Edge（WPS 仍在放映） | Edge 被 WPS 全屏覆盖 |
| 监视器先 `View.Exit()` 再启动 Edge | **有效，Edge 在最前** |
| Edge 关闭后 `Run() + GotoSlide(target)` | **有效** |

**结论**：WPS 全屏放映是特殊覆盖层，普通置顶不可靠；模式 B/B2 必须先退出放映。

## 7. E6：Windows 前台锁

**现象**：`bring-log.txt` 记录：

```text
set 1513712 pos=True fg=False
```

窗口已置顶，但 `SetForegroundWindow` 返回 False，键盘输入仍发往旧窗口。

**原因**：后台 `wscript.exe` / 隐藏 `powershell.exe` 不满足 Windows 前台激活条件。

**修复**：

1. 直接 `SetForegroundWindow`；
2. `SwitchToThisWindow`；
3. `AttachThreadInput` 前台/目标线程 + `BringWindowToTop` + `SetFocus`；
4. 模拟 Alt 键按下/抬起，解锁前台锁；
5. 校验 `GetForegroundWindow`。

**自检**（`test-focus.ps1`，本机实测）：

```text
orig=789718 test=2172088 force=True fg=2172088 match=True restored=True
PASS: foreground activation works
```

**结论**：前台锁是模式 B/B2 的核心可靠性问题；必须用 Win32 手段，并保留 `test-focus.ps1` 作为目标机预检。

## 8. E7：黑场过渡时序

`black-fade.ps1` 参数：`FadeMs=300`、`HoldMs=1000`、`MaxHoldMs=3000`。

实测日志（触发侧）：

```text
fade in  21:52:17.213
hold     21:52:17.498
fade out 21:52:18.552  held=1039 ready=True
done     21:52:18.862
```

- 渐入 285ms；保持 1039ms；渐出 310ms；总计约 1.65s。
- 切换动作（agent session）在 `21:52:17.616`，即 **hold 之后 118ms**；确认“全黑后才切换”。
- 截图验证：黑屏时全屏截图 `mean=0`；淡出后恢复正常画面。
- Edge 慢时黑场会保持到 `cover-ready`（最多 3s），不会提前露出桌面。

**结论**：0.3s 渐黑 + 1.0s 保持 + 0.3s 渐透明可以作为“最短 1.6s”的基线；实际时长由内容就绪时间决定。

## 9. E8：返回侧提前信号

**未加信号时**：Edge 窗口关闭后才起黑场，屏幕会先露出 WPS 编辑器/桌面。

**加入标题信号后**：

```text
agent return requested   21:52:28.890
cover fade in            21:52:29.038
cover hold               21:52:29.346
bring-wps focused        21:52:30.947
agent session end        21:52:30.999   # Edge 窗口关闭
cover fade out           21:52:31.630  held=2433 ready=True
cover done               21:52:31.946
```

- 黑场在全黑后、WPS 恢复时、Edge 关闭前一直保持；
- WPS 在 Edge **仍打开**时恢复并聚焦；
- 页面关闭后再淡出到目标页；
- 全程未露出 WPS 编辑器/桌面。

**代价**：返回侧黑场约 2–3s（取决于 WPS 恢复和页面 2s 延迟），比触发侧的 1.6s 长。

**结论**：返回侧要无缝，必须有“关闭前信号”；`document.title = TEF-LIVE-RETURN` + 延迟 `window.close()` 是一种低成本实现。

## 10. E9：Edge 同步/隐式登录弹窗

用户现场截图显示的弹窗：

> 我们现在正在你的所有设备上同步你的浏览数据
> `2945707804@qq.com` 已在此设备上登录，因此我们也使你登录到了 Microsoft Edge。

对照实验：

| 条件 | 结果 |
|------|------|
| 全新 profile + 原参数（`--no-first-run --no-default-browser-check`） | 弹窗出现 |
| 全新 profile + 参数组 | 弹窗消失，HTML 正常 |
| 真实短版 watcher + 全新 `tef-live-trustedzones` profile | 弹窗消失，返回黑场正常 |

参数组：

```text
--disable-sync
--disable-features=msImplicitSignin,msEdgeSync,msFirstRunExperience,msEdgeFirstRunExperience,EdgeSync
--disable-background-networking --disable-component-update --disable-extensions
--disable-default-apps --no-service-autorun
--disable-client-side-phishing-detection --disable-domain-reliability
--no-pings --disable-breakpad
```

**结论**：启动参数组在该环境有效；它不是微软文档承诺的稳定接口，仍需策略兜底：

- `SyncDisabled=1`
- `ImplicitSignInEnabled=0`
- `HideFirstRunExperience=1`

策略位于 `HKLM\SOFTWARE\Policies\Microsoft\Edge`，会作用于该用户/机器的所有 Edge，需要管理员权限，并可能显示“由组织管理”。

## 11. E10：WPS COM 瞬时错误与自动清理

**故障注入**：触发 Edge 后强制结束 `wpp.exe`。

日志：

```text
COM busy: err=462 ... count=1
...
COM busy: err=462 ... count=20
WPS COM unavailable after retries; exiting
cleanup start
cleanup done
```

- 重试 20 次，约 2 秒；
- 连续失败后执行 `Cleanup`：结束 `tef-live-*` Edge、focus-agent、black-fade、bring-wps，删除锁/标志；
- 检查结果：`watchers=0 edge=0 helpers=0 lock=False`。

**结论**：旧版把任何 COM 错误当成“WPS 已关闭”立即退出，是概率性闪退的主要诱因；重试 + 清理可以把它变成可观测、可恢复的失败路径。

## 12. E11：空格键事件兼容

自动化发送空格时，浏览器里可能出现 `e.code` 为空、`e.key` 为空格的情况。页面如果只判断 `e.code === "Space"`，会收不到。

修复：同时判断：

```js
if (e.code === "Space" || e.key === " ") { ... }
```

真实键盘和合成按键都能工作。

## 13. 其他观察

- Edge 全屏启动时会显示“若要退出全屏……”提示，这是正常 UI，不是同步弹窗，会自动消失。
- WPS 的放映窗口类名可能是 `Qt5QWindowIcon`，标题为 `WPS Presentation Slide Show - [...]`；`bring-wps.ps1` 不能只依赖 PowerPoint 的 `screenClass`。
- WPS 可能不公开 `SlideShowWindow.HWND`，需要枚举 `wpp.exe`/`wps.exe`/`POWERPNT.EXE` 的全屏窗口并按特征打分。
- 多显示器/DPI 缩放会影响窗口尺寸判断；当前用主屏虚拟化尺寸 + 0.9 容忍度。
- 多个 watcher 实例会导致重复触发；必须保留心跳锁，并且只有持锁实例才允许清理。

## 14. 待验证问题

1. 同一套模式 A/B/B2 在 Microsoft PowerPoint（非 WPS）下的行为。
2. WPS 其他版本（11.x、13.x）对视频时长、全屏覆盖层、COM 错误码的差异。
3. Edge 参数组在不同 Edge 版本/策略下的稳定性。
4. 返回侧能否把 2–3s 黑场压缩到更接近 1.6s，同时不露出 WPS 恢复过程。
5. 是否值得引入键盘钩子，把触发侧“先翻下一页”的 100ms 缝隙也消除。
6. 跨机分发的字体、DPI、安全软件差异如何自动预检。
