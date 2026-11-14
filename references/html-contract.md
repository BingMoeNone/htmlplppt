# HTML 分步契约与 capture 模式

## 1. 契约

页面必须暴露：

```js
window.__tefSteps = {
  total: 8,
  durations: [1800, 3200, 3200, 3200, 3200, 3200, 3200, 2800],
  seek(step, tMs) {}
};
```

约定：

- `step` 为 `0..total-1` 的整数。
- `tMs` 取 `[0, durations[step]]`。
- `seek(step, 0)` 必须渲染“上一步稳定末态”；`seek(step, duration)` 必须渲染“本步稳定末态”。
- `seek` 同步完成 DOM/样式更新，不依赖 `requestAnimationFrame`、`setTimeout` 或真实时间。
- 相邻步骤边界帧必须逐像素一致：`seek(i, durations[i])` 与 `seek(i+1, 0)` 渲染结果相同。

## 2. 步骤模型

典型映射（以连续时间轴 `clock` 为例）：

| step | 含义 | clock 区间 | duration |
|------|------|------------|----------|
| 0 | 片头 | `[-INTRO, 0]` | `INTRO` |
| 1..N | 主体步骤 | `[(s-1)*DUR, s*DUR]` | `DUR` |
| N+1 | 收束 | `[N*DUR, N*DUR+OUTRO]` | `OUTRO` |

实现要点：

```js
function stepStart(step) {
  if (step === 0) return -INTRO;
  if (step <= BODY.length) return (step - 1) * DUR;
  return BODY.length * DUR;
}

function stepDuration(step) {
  if (step === 0) return INTRO;
  if (step <= BODY.length) return DUR;
  return OUTRO;
}

function seek(step, tMs) {
  step = Math.max(0, Math.min(TOTAL - 1, Math.floor(Number(step) || 0)));
  const local = Math.max(0, Math.min(stepDuration(step), Number(tMs) || 0));
  clock = stepStart(step) + local;
  renderAt(clock);       // 同步更新所有由 clock 决定的视觉状态
  if (CAPTURE) seekCssAnimations(clock + INTRO);
}
```

## 3. capture 模式

`?capture=1` 时必须：

- 不启动自动 RAF 循环、不自动播放。
- 不监听键盘、鼠标、点击、指针视差。
- 不触发自动循环/重置定时器。
- 卡片 tilt/spot 固定为 0。
- 状态标签由 `clock` 推导，保证边界帧文本一致。

建议隐藏交互元素：

```css
html.is-capture .studio-hint,
html.is-capture .head-right .icon-btn { display: none; }
```

## 4. CSS 动画的确定性定位

CSS keyframe 动画不是由 `seek` 自动控制的。capture 模式下：

1. 禁用 transition，避免类切换产生不可定位的中间态：

```css
html.is-capture *,
html.is-capture *::before,
html.is-capture *::after {
  transition-duration: 0s !important;
  transition-delay: 0s !important;
}
```

2. 对全局 keyframe 动画逐帧定位：

```js
function seekCssAnimations(globalMs) {
  for (const animation of document.getAnimations()) {
    try {
      animation.pause();
      const end = animation.effect.getComputedTiming().endTime;
      animation.currentTime = Number.isFinite(end) ? Math.min(globalMs, end) : globalMs;
    } catch {
      // Ignore animations that no longer have an effect.
    }
  }
}
```

3. 关闭“步骤边界处重新触发”的局部动画，否则上一步末帧与下一步首帧不一致：

```css
html.is-capture .detail-copy.is-in,
html.is-capture .card.is-complete .detail-seal {
  animation: none !important;
}
```

代价是详情淡入和完成印章弹出效果丢失；这是保证边界逐像素一致的取舍。若必须保留，需为这些动画单独实现可 seek 的进度，并接受更复杂的边界同步。

4. 捕获入口调用一次 `restartIntro()`（重加 `is-live`），强制一次样式刷新，然后 `seek` 只推进时间，不重新触发入场。

## 5. 字体预检

视频会把字体烘焙进像素。捕获前必须确认：

```bash
fc-match "Microsoft YaHei"   # 必须命中 msyh.ttc / Microsoft YaHei
```

未命中时 `capture.mjs` 直接失败。字体安装示例：

```bash
mkdir -p ~/.local/share/fonts
cp /mnt/c/Windows/Fonts/msyh.ttc /mnt/c/Windows/Fonts/msyhbd.ttc \
   /mnt/c/Windows/Fonts/msyhl.ttc /mnt/c/Windows/Fonts/Deng.ttf \
   /mnt/c/Windows/Fonts/Dengb.ttf ~/.local/share/fonts/
fc-cache -f ~/.local/share/fonts
```

## 6. 常见失败

| 现象 | 原因 | 修复 |
|------|------|------|
| 边界帧不一致 | detail/seal 动画在边界重新触发 | capture 下关闭这些动画 |
| 帧内容重复、`seek` 抛错却成功退出 | 未检查 `Runtime.evaluate` 的 `exceptionDetails` | 捕获器统一检查并抛错 |
| 捕获到空白页 | 双重导航导致 load 事件竞态 | 先 about:blank，再单次 `Page.navigate` |
| 字体错乱 | 未安装目标字体 | 先跑字体预检 |
| 旧帧混入 | 重跑未清空 frames 目录 | 每步写帧前删除目录 |
| 交互模式与 capture 不一致 | 两套渲染路径 | 共用同一个 `renderAt(clock)` |
