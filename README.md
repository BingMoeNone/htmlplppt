# htmlplppt

**HTML 动画 × PowerPoint/WPS 结合技术研究**

> 状态：研究项目初始化（2026-11-14）
> 目标：把“HTML 动画如何进入 PPT”从一个演示脚本，整理成可复现、可评估、可扩展的技术栈。

## 1. 为什么单独开这个项目

HTML 动画和 PPT 是两种完全不同的运行时：

- HTML/CSS/JS 有完整的动画、交互和实时状态；
- PowerPoint/WPS 的放映态只保证有限的媒体与动画能力，且不同版本行为不一致；
- 现场环境通常禁止宏、联网插件和额外安装。

这个项目研究的是**边界工程**：在不改 PPT 运行时、不依赖宏的前提下，让 HTML 动画的“过程”稳定地出现在答辩/演示现场，并能量化它的保真度、延迟和失败模式。

## 2. 三条技术路线

| 路线 | 原理 | 适用 | 主要代价 |
|------|------|------|----------|
| **模式 A：单页分步视频** | 把 HTML 动画拆成 N 个确定性步骤，逐步捕获并编码为 MP4，全部叠在同一张幻灯片上，用原生单击动画逐段播放 | 页数受限、片段短（WPS 下单段建议 ≤0.9s）、需要离线单文件 | 需要捕获/编码/组装工具链；不是实时页面 |
| **模式 B：实时浏览器** | 本地启动器/监视器在目标页退出 WPS 放映，用 Edge `--app` 全屏打开同目录 HTML；关闭后回到 PPT | 需要完整 HTML 交互、WPS 单页长视频失效 | 需要同目录文件；首次可能有安全提示 |
| **模式 B2：现有内容页空格触发 + 黑场** | 监视器轮询放映页；在目标页按一次空格触发对应 HTML；全屏黑场覆盖 WPS↔Edge 切换；返回时在黑场下恢复 WPS | 不能新增页、不能覆盖原页面内容、希望“只按空格” | 需要 Windows + WPS/Edge；返回侧黑场通常比触发侧稍长 |

决策表与详细实现见 [docs/01-architecture.md](docs/01-architecture.md)。

## 3. 已沉淀的关键发现

这些结论来自 WPS 12.1 + Microsoft Edge 的实测，详细证据见 [docs/02-experiments-and-findings.md](docs/02-experiments-and-findings.md)：

- WPS 12.1 的 native 媒体点击序列对单段时长敏感：2 段 × 0.816s 正常，2 段 × 1.816s/2.45s 会把空格当作翻页；8 段 × 0.45–0.83s 正常。因此 WPS 目标需要把每段压到 ≤0.9s。
- 捕获必须是**逐帧 `seek` + 截图**，不是实时录屏；边界比较使用捕获源 PNG，不是解码后的 MP4 帧。
- WPS 全屏放映覆盖层无法被普通置顶窗口可靠覆盖；模式 B/B2 必须**先退出放映**再让 Edge 接管。
- Windows 前台锁会拒绝后台脚本直接 `SetForegroundWindow`（实测 `fg=False`）；需要 `AttachThreadInput` + 模拟 Alt 键，并由常驻 agent 预热。
- 黑场过渡可用 0.3s 渐黑 + 1.0s 保持 + 0.3s 渐透明（最短 1.6s）；切换动作必须等全黑后再执行，且要等 Edge/WPS 真正就绪再淡出。
- 返回侧如果等 Edge 关闭后才起黑场，会先露出 WPS 编辑器；解决方式是 HTML 先把标题改成 `TEF-LIVE-RETURN` 并延迟关闭，让 watcher 在 Edge 关闭前起黑场并恢复 WPS。
- Edge 的“同步/隐式登录”弹窗可以用 `--disable-sync` + `--disable-features=msImplicitSignin,...` 参数组抑制；官方兜底是 `SyncDisabled` / `ImplicitSignInEnabled` 策略。
- WPS COM 在切换/恢复时可能出现瞬时错误（实测 `err=462`）；应重试并记录 `COM busy`，连续失败后自动清理 Edge/agent/黑场，而不是立即闪退。

## 4. 目录

```text
htmlplppt/
  README.md
  docs/
    00-research-agenda.md      研究问题、指标与实验设计
    01-architecture.md         三条路线、分层架构与约束
    02-experiments-and-findings.md  实测记录与结论
    03-roadmap.md              研究路线图与风险
    04-reproduce.md            环境、命令、验收与排障
    05-playbook.md             完整操作手册（来自已验证 skill）
    06-tooling.md              工具链说明
  references/
    html-contract.md           HTML 分步契约
    pptx-timing.md             native/appear 时序与 OOXML
    compatibility.md           WPS/Office 兼容性与降级
    live-browser-mode.md       模式 B 实时浏览器
    live-content-trigger.md    模式 B2 空格触发 + 黑场
  scripts/                     捕获、编码、组装、监视器、聚焦/黑场
  templates/                   stepwise-demo.html、config 示例
  vendor/                      python-pptx 1.0.2（离线组装依赖）
```

## 5. 快速开始

### 模式 A（Linux 侧捕获/编码/组装）

```bash
bash scripts/preflight.sh
# 可选：python-pptx 已在 vendor/，需要重建时运行
bash scripts/setup.sh

node scripts/check-contract.mjs --html templates/stepwise-demo.html
node scripts/capture.mjs --html templates/stepwise-demo.html --out out/capture --width 1920 --height 1080 --fps 60
node scripts/encode.mjs --manifest out/capture/manifest.json --out out/clips --crf 14
node scripts/compress-clips.mjs --manifest out/clips/manifest.json --out out/clips-wps --max 0.9
python3 scripts/verify.py --capture out/capture/manifest.json --clips out/clips/manifest.json
PYTHONPATH=vendor python3 scripts/build_pptx.py --source deck.pptx --clips out/clips-wps/manifest.json --after 12 --mode native --out deck-animated.pptx
PYTHONPATH=vendor python3 scripts/verify_pptx.py --source deck.pptx --out deck-animated.pptx --new-index 13 --expected-slides 20 --expected-videos 8 --mode native
```

注意：完整捕获需要 Chrome、ffmpeg/ffprobe、Pillow 和目标 CJK 字体；`preflight.sh` 会逐项检查。

### 模式 B / B2（Windows 现场）

```text
demo/
  deck.pptx
  watch-live-content.vbs      # 模式 B2 通用模板
  focus-agent.ps1
  black-fade.ps1
  bring-wps.ps1
  focus-lib.ps1
  page1.html / page2.html / page3.html
```

1. 修改 `watch-live-content.vbs` 顶部 CONFIG（deck、目标页、HTML、profile、标签）。
2. 在目标机先跑 `powershell -ExecutionPolicy Bypass -File scripts/test-focus.ps1`，看到 `PASS`。
3. 双击 `watch-live-content.vbs`；首次如提示安全警告，选择允许。
4. 在目标页按一次空格触发 HTML；空格推进，最后一步空格或 Esc 返回。
5. 应急停止：双击 `scripts/stop-live.vbs`。

完整配置、黑场/返回信号、Edge 同步参数与验收清单见 [references/live-content-trigger.md](references/live-content-trigger.md)。

## 6. 依赖

- Node.js 24+（捕获/编码脚本使用内置 WebSocket/fetch）
- Google Chrome / Chromium
- ffmpeg / ffprobe
- Python 3.12+，python-pptx 1.0.2（已在 `vendor/`），Pillow
- 模式 B/B2：Windows 10/11 + WPS Office（实测 12.1）+ Microsoft Edge

## 7. 技术资产来源

本项目的脚本、参考文档和实测结论来自 LyMtrix-TEF 项目的以下实现，整理为独立研究副本：

- `.claude/skills/html-animation-to-pptx/` — 当前已验证的 skill 与模板
- `docs/competition/live-browser/` — 模式 B 实时浏览器实现
- `docs/competition/完整最终版/初赛短版本/` — 模式 B2 空格触发 + 黑场 + 自动清理的验证实现

项目中的通用模板与案例特定脚本的区别：

- `scripts/watch-live-content.vbs`、`scripts/black-fade.ps1`、`scripts/focus-agent.ps1`、`scripts/stop-live.vbs` 是通用研究资产；
- `scripts/watch-slides.vbs` 保留 LyMtrix 20 页 live 稿的参考实现（目标页 13、V2.html），可作为案例研究，不要直接套用到其他 deck。

## 8. 研究文档

- [研究议程](docs/00-research-agenda.md)
- [架构与路线](docs/01-architecture.md)
- [实验与发现](docs/02-experiments-and-findings.md)
- [路线图](docs/03-roadmap.md)
- [复现指南](docs/04-reproduce.md)
- [完整 playbook](docs/05-playbook.md)
- [工具链说明](docs/06-tooling.md)

## 9. 边界

- 不修改 PowerPoint/WPS 运行时，不依赖 VBA 宏、ActiveX 或联网插件。
- 模式 A 的产物是视频 + PPTX；模式 B/B2 的产物是 PPTX + 本地 HTML/启动器。
- 本项目不承诺跨平台一致；Windows 是模式 B/B2 的目标环境。
- 生成物（`out/`、PPTX、ZIP、日志）不进入 git。
