# 架构：三条路线与分层组件

> 状态：架构设计
> 日期：2026-11-14
> 关联：[研究议程](00-research-agenda.md)、[实验与发现](02-experiments-and-findings.md)、[复现指南](04-reproduce.md)

## 1. 总原则

1. **PPT 是展示/控制面，不是 HTML 运行时。** 不假设 PowerPoint/WPS 能直接执行任意 JS。
2. **确定性优先。** 能离线的过程（模式 A）尽量离线；必须实时的部分（模式 B/B2）用本地进程桥接。
3. **不依赖宏和联网插件。** 只用 PPT 原生能力、文件系统、本地 Edge/WPS COM。
4. **每个切换点都要有可观测信号。** 黑场就绪、Edge 聚焦、WPS 恢复、进程关闭都要有标志或日志。
5. **失败可恢复。** 进程要有心跳、超时、自动清理和人工应急入口。

## 2. 路线选择

```text
需要把 HTML 动画放进 PPT
  ├─ 片段短、页数受限、要单文件/离线
  │    └─ 模式 A：确定性捕获 → 分步视频 → 原生单击动画
  ├─ 需要完整 HTML 交互/长动画
  │    ├─ 有独立启动页/可新增页
  │    │    └─ 模式 B：自动监视器/按钮启动 Edge，关闭后回 PPT
  │    └─ 不能新增页/不能覆盖原内容
  │         └─ 模式 B2：现有内容页空格触发 + 黑场 + 返回信号
  └─ 只要静态画面
       └─ 高 DPI 截图/PDF 矢量导出
```

| 维度 | 模式 A | 模式 B | 模式 B2 |
|------|--------|--------|---------|
| PPT 页数变化 | 只 +1 页 | 可 +1 页（启动页） | 0 页 |
| HTML 改造量 | 需要 `__tefSteps` 契约 | 基本不改，需关闭逻辑 | 需空格/返回信号契约 |
| 现场依赖 | Chrome/ffmpeg（生成侧） | Windows + Edge/WPS | Windows + Edge/WPS |
| 交互能力 | 无（视频） | 完整 | 完整 |
| 切换延迟 | 进页即播/空格 | 退出放映→Edge | 黑场下切换 |
| 失败模式 | 视频时长/编码/字体 | 焦点/全屏覆盖/安全提示 | 焦点/黑场时机/COM |

## 3. 模式 A：捕获/编码/组装流水线

```text
HTML 页面（契约：__tefSteps）
  │  check-contract.mjs
  ▼
Chrome 无头 + CDP
  │  capture.mjs：逐帧 seek + 截图（非录屏）
  ▼
帧序列 + manifest
  │  encode.mjs：H.264 / yuv420p / 60fps / 无音轨 / faststart
  ▼
每步 MP4 + 首帧 PNG
  │  compress-clips.mjs（WPS 目标）：压到 ≤0.9s，保留转场与末帧
  ▼
clips-wps/manifest.json
  │  verify.py：帧数/编码/分辨率/时长/边界源 PNG 比较
  ▼
build_pptx.py（native/appear）
  │  单页叠放：首段 delay=0，其余 indefinite；透明海报帧
  ▼
deck-animated.pptx
  │  verify_pptx.py：页数/位置/原页 C14N/视频数/动画模式
  ▼
wps_smoke.ps1：能打开、首帧正确（不证明空格分步，需人工验收）
```

关键设计：

- **捕获不是录屏**：每次 `seek(step, tMs)` 后同步截图，帧的时间轴由契约决定，不受渲染抖动影响。
- **边界比较用源 PNG**：MP4 有损编码不可能像素一致；比较对象是捕获阶段的两张 PNG。
- **native 是默认模式**：使用 python-pptx 生成的 `p:video` 时序，兼容性最好；appear 是后备，注入 `p:seq`。
- **WPS 时长压缩是编译期步骤**：把“停留”留给演讲者按空格控制，视频只保留转场。

## 4. 模式 B：实时浏览器桥接

```text
watch-slides.vbs（或按钮启动器）
  ├─ 启动 WPS、打开 deck、从第 1 页放映
  ├─ 轮询 CurrentShowPosition
  ├─ 到目标页：View.Exit() 退出全屏覆盖层
  ├─ 启动 Edge --app=file:///.../V2.html --start-fullscreen
  ├─ bring-edge.ps1 持续置顶/聚焦
  ├─ HTML 内空格推进；最后一步/Esc → window.close()
  └─ Edge 关闭 → Run() + FocusShow + GotoSlide(target) 回 PPT
```

关键设计：

- **先退出 WPS 放映再启动 Edge**：WPS 全屏覆盖层无法被普通置顶窗口可靠覆盖。
- **HTML 标题固定**：例如 `TEF-LIVE-STAGE`，用于聚焦助手定位窗口。
- **前台锁处理**：`focus-lib.ps1` 使用 `AttachThreadInput` + 模拟 Alt 键；`bring-edge.ps1` 持续校验。
- **返回焦点**：`bring-wps.ps1` 枚举 `wpp.exe`/`wps.exe`/`POWERPNT.EXE` 的全屏窗口并聚焦。

## 5. 模式 B2：现有内容页空格触发 + 黑场

### 5.1 组件

```text
watch-live-content.vbs（watcher）
  ├─ black-fade.ps1       全屏黑场：渐黑/保持/渐透明
  ├─ focus-agent.ps1      常驻聚焦 agent：预热、ready/exit/return 信号
  ├─ bring-wps.ps1        恢复后聚焦 WPS/PowerPoint 放映窗口
  ├─ focus-lib.ps1        Win32 窗口查找/前台解锁
  └─ stop-live.vbs        应急停止
```

### 5.2 触发（PPT → Edge）

```text
目标页等待空格
  ├─ WPS 翻到下一页（watcher 检测 pos > prevSlide）
  ├─ 写 cover-fade.flag（渐黑 0.3s）
  ├─ 等 350ms（确保全黑）
  ├─ 写 agent-arm.flag + 启动 Edge（独立 --user-data-dir）
  ├─ agent 找到/聚焦 Edge → 写 cover-ready.flag
  ├─ 退出 WPS 放映
  └─ 黑场等 cover-ready 且 ≥1s → 渐透明 0.3s
```

### 5.3 返回（Edge → PPT）

```text
HTML 最后一步空格 / Esc
  ├─ document.title = TEF-LIVE-RETURN
  ├─ 页面内渐黑 0.3s
  ├─ 延迟 2s 后 window.close()
  ├─ agent 轮询标题 → 写 return-request.flag
  ├─ watcher 写 cover-fade.flag，等 350ms
  ├─ ResumeShow（WPS 在 Edge 仍打开时恢复）
  ├─ 页面关闭 → watcher 写 cover-ready.flag → 渐透明
  └─ 露出已恢复的目标页
```

### 5.4 状态与去重

- `prevSlide`：当前放映页。
- `doneFlags`：每个目标页在本场放映中只触发一次；返回后再次按空格只翻页。
- `cover-ready.flag`：内容真正就绪后才允许淡出；最多保持 3s。
- `returnPrepared`：返回请求已处理，避免 Edge 关闭后重复恢复。

## 6. 可靠性层

| 机制 | 解决的问题 |
|------|------------|
| watcher 心跳锁 | 单实例；崩溃后帮助进程可感知 |
| `focus-agent` / `black-fade` 心跳 | watcher 失联 30s 后自动退出，避免残留 |
| COM 错误重试 | WPS 切换/恢复时的瞬时 `err=462` 不再导致闪退 |
| `Cleanup` | 最终失败时结束 `tef-live-*` Edge、agent、黑场、helper，删除锁/标志 |
| 持锁守卫 | 第二个 watcher 预检失败时不会误杀正在运行的实例 |
| Edge profile 隔离 | 每个 HTML 独立 `--user-data-dir`，避免窗口归属歧义 |
| Edge 同步参数组 | 抑制隐式登录/同步弹窗 |
| 安全提示最小化 | 只有首次启动 watcher 可能需要允许一次 |

## 7. 文件与职责

```text
scripts/
  capture.mjs / encode.mjs / compress-clips.mjs    模式 A 捕获/编码
  build_pptx.py / verify.py / verify_pptx.py        模式 A 组装/校验
  check-contract.mjs                                HTML 契约预检
  preflight.sh / setup.sh                           模式 A 环境/依赖
  watch-slides.vbs                                  模式 B 自动监视器（案例）
  watch-live-content.vbs                            模式 B2 通用模板
  black-fade.ps1                                    模式 B2 黑场
  focus-agent.ps1                                   模式 B2 常驻聚焦
  focus-lib.ps1                                     聚焦/窗口查找库
  bring-edge.ps1 / bring-wps.ps1                    Edge/WPS 聚焦助手
  test-focus.ps1                                    前台聚焦自检
  stop-live.vbs / stop-watch.vbs                    应急停止
  preflight-live.ps1                                Edge/文件/COM 预检
  build_auto_deck.py / build_launcher_deck.py       启动页生成
  templates/stepwise-demo.html                      2 步确定性样例
```

## 8. 约束与假设

- 模式 B/B2 的现场是 Windows + WPS（实测 12.1）+ Edge；PowerPoint 兼容性需要单独验证。
- 所有本地文件必须同目录；HTML 不能依赖外部网络。
- 模式 A 的字体是硬门槛：捕获前必须命中目标 CJK 字体。
- 不保证所有 WPS 版本行为一致；需要保留版本矩阵和降级方案。
- 安全软件/组策略可能阻止本地脚本、窗口焦点或 Edge 参数；需要现场预检与人工兜底。
