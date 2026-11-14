# 现场兼容性验证与降级

## 1. 验证矩阵

每次交付至少记录下面四项；下面的“已验证”只是示例，不代表你的环境：

| 环境 | native | appear | 人工空格分步 | 备注 |
|------|--------|--------|--------------|------|
| Windows WPS 12.1 | 已验证通过 | 待测 | 已验证通过 | `Visible=false` 会抛 COMException，需回退 |
| Windows Office 2016/2019/365 | 已验证通过 | 待测 | 已验证通过 | 需在实际答辩机器复测 |
| macOS PowerPoint | 未验证 | 未验证 | 未验证 | 视频与动画时序可能不同 |

验收口径：

1. 打开无“修复”提示。
2. 进入动画页，第 1 段自动播放（或按一次空格开始）。
3. 每次空格播放下一段。
4. 最后一段播完后，再按空格翻到下一页。
5. 切换处无黑屏、无跳变、无可见播放控件残留。

## 1b. WPS 单页视频时长限制（实测）

| 单页视频 | WPS 12.1 结果 |
|----------|---------------|
| 2 段 × 0.816s | 正常：空格逐段播放 |
| 2 段 × 1.816s / 2.45s | 失败：空格直接翻页 |
| 8 段 × 0.45–0.83s | 正常：8 步逐段播放 |

结论：WPS 的 native 媒体点击序列对单段时长敏感。超过约 1 秒时，WPS 可能不再把该媒体当作“单击停点”，空格直接翻页。这不是 timing XML 写错——2 秒与 0.8 秒的结构完全相同。

修复：用 `scripts/compress-clips.mjs` 把每段压到 ≤0.9 秒：

```bash
node scripts/compress-clips.mjs --manifest out/clips/manifest.json --out out/clips-wps --max 0.9
```

- 使用 ffmpeg `-itsscale` 缩放时间戳，仍输出 60fps。
- 每段只保留转场动画，末帧保持；停留由演讲者按空格控制。
- 压缩后用 `out/clips-wps/manifest.json` 组装。
- 不要用实时屏幕录制来“缩短”片段；仍走逐帧捕获 + 编码。
- Office-only 目标可先不压缩；若 Office 也出现同样问题，同样适用。

## 2. 自动冒烟（只做初步筛除）

`scripts/wps_smoke.ps1`：

```bash
WIN_TMP=/mnt/c/Users/<User>/AppData/Local/Temp
cp scripts/wps_smoke.ps1 "$WIN_TMP/wps_smoke.ps1"
cp out/sample-lv6-2steps-native.pptx "$WIN_TMP/sample-native.pptx"
/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Users\<User>\AppData\Local\Temp\wps_smoke.ps1' \
  -Path 'C:\Users\<User>\AppData\Local\Temp\sample-native.pptx' \
  -Out 'C:\Users\<User>\AppData\Local\Temp\slide13-native.png'
```

脚本会：

- 用 `Kwpp.Application` 打开文件；
- 断言页数；
- 导出目标页为 1920×1080 PNG。

预览图检查：优先把导出图与**源首帧**比较，而不是用固定的亮度阈值。深色片头（卡片入场前）本身就可能 mean < 5，固定阈值会误报。

```python
from PIL import Image, ImageChops
src = Image.open('out/capture/frames/step-01/00000.png').convert('RGB')
preview = Image.open('slide13-native.png').convert('RGB')
assert preview.size == src.size, (preview.size, src.size)
diff = ImageChops.difference(src, preview)
assert diff.getbbox() is None, f'导出预览与源首帧不一致: {diff.getbbox()}'
```

在 WPS 12.1 + native 模式下，最终 PPT 第 13 页预览与源首帧实测逐像素一致（`mean_abs_diff=0.0`）。若播放器有轻微缩放/色彩差异，可改为允许很小的平均绝对误差，但不应出现大片黑色或透明海报变黑块。固定 `mean > 5` 只适用于首帧已有明显卡片的场景。

注意：

- **自动冒烟不证明放映态空格分步**。它只证明文件能打开、目标页有内容。
- appear 模式的编辑态预览会把所有海报叠放，不能代表放映态。
- WPS 12.1 设置 `$app.Visible = $false` 可能抛 `COMException E_FAIL (0x80004005)`；捕获后回退 `Visible = $true`，并在文档中记录。
- `Slide.Export(path, 'PNG', 1920, 1080)` 在 WPS 12.1 可用；若不可用，可尝试 `Presentation.SaveAs(dir, 18)`，但必须明确记录偏差。

## 3. 人工验收步骤

1. 断网、关闭宏、关闭所有加载项。
2. 用 WPS 打开 2 步样例，定位到动画页，按 F5 从当前页放映。
3. 观察第 1 段是否自动播放。
4. 按空格，观察第 2 段是否播放。
5. 再按空格，观察是否翻到下一页。
6. 重复到最后一页，确认无修复提示、无卡顿、无黑边。
7. 在 Office 上重复。
8. 记录环境版本、结果、异常现象。

## 4. 降级顺序

| 顺序 | 方案 | 触发条件 | 代价 |
|------|------|----------|------|
| 1 | native | 默认 | 无 |
| 2 | appear | native 在目标环境不按空格分步 | 需要注入 `p:seq`，兼容性风险更高 |
| 3 | 每步静态 PNG + 原生淡入 | 两种视频模式都失败 | 丢失步内动效 |
| 4 | 整段单视频进页自动播放 | 只求能放，不求分步 | 放弃空格步进 |

降级必须在渲染全量片段之前决定，避免产出无法播放的资产。

## 5. 性能与体积

- 默认 1920×1080、60fps、CRF 14；暗色渐变若出现色带，降到 CRF 12 或加轻微抖动。
- 低配机器优先 1080p/30fps；1440p 只在高性能机器上使用。
- 每段开头 100–200ms 静止首帧，掩盖解码首帧延迟。
- `+faststart` 让播放器快速起播。
- 目标成品 ≤ 50MB；超限先降码率/分辨率，不减少步骤。
- 中途按空格会跳到下一段的稳定起始状态，这是步进语义，不是缺陷；现场提示“等当前段结束再按空格”。

## 6. 常见环境问题

| 问题 | 处理 |
|------|------|
| 打开提示“修复” | timing XML 或关系不合法；先用 2 步样例验证，不要全量渲染 |
| 空格翻页但不播视频 | 点击停点缺失或模式错误；检查 native/appear 门禁 |
| 首段不自动播放 | 首媒体节点 `delay` 不是 0 |
| 第 2 段开始黑屏 | 透明海报被渲染成黑块；改 appear 模式 |
| 只播放第一段 | 媒体节点顺序/数量不对；检查 `p:video` 数量与 spid |
| 播放控件残留 | 鼠标悬停触发；现场移开鼠标，必要时隐藏控制条 |
| WPS 打开报 COM 错误 | 文件损坏或版本不支持；回退 Office 验证 |
