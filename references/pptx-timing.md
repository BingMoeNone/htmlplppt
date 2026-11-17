# 单页分段视频与 PowerPoint 时序

## 1. 页面结构

- 只新增 1 页；所有步骤共用一个页面。
- 每个步骤一个视频形状，位置和尺寸完全一致，铺满整页：
  `(0, 0, 12192000, 6858000)` EMU（16:9）。
- z 序：第 1 段在最底层，第 N 段在最顶层。
- 第 1 段海报帧用真实首帧；第 2..N 段用全透明 PNG 海报帧，避免未播放时遮住上一段。
- 视频编码：H.264、yuv420p、无音轨、`+faststart`；每段开头保留 100–200ms 静止首帧，掩盖解码首帧延迟。

## 2. native 模式（默认）

python-pptx 的 `shapes.add_movie()` 会：

- 在幻灯片中创建 `p:pic`，其中 `p:nvPr/a:videoFile` 与 `p:extLst/p14:media` 指向嵌入的 MP4；
- 在 `p:timing` 中为每个视频创建一个 `p:video/p:cMediaNode` 节点，默认：

```xml
<p:video>
  <p:cMediaNode vol="80000">
    <p:cTn id="2" fill="hold" display="0">
      <p:stCondLst><p:cond delay="indefinite"/></p:stCondLst>
    </p:cTn>
    <p:tgtEl><p:spTgt spid="2"/></p:tgtEl>
  </p:cMediaNode>
</p:video>
```

`delay="indefinite"` 表示“单击时播放”。按添加顺序排列，PowerPoint 会把它们当作依次的单击停点。

要做的只有两件事：

1. 把第 1 个媒体节点的 `p:cond@delay` 改为 `"0"`（进页自动播放）。
2. 第 2..N 段使用全透明海报帧。

结果：进页播第 1 段；每次空格触发下一个媒体节点；最后一段播完后，再按空格翻页。

> 不要把 native 模式和 appear 模式混淆：native 模式不注入 `p:seq`，也不使用 Appear/`playFrom`。

### 透明海报帧

用 Pillow 生成 1920×1080 全透明 PNG：

```python
from PIL import Image
Image.new('RGBA', (1920, 1080), (0, 0, 0, 0)).save('transparent.png')
```

`add_movie(..., poster_frame_image='transparent.png')`。若目标播放器把透明海报渲染成黑块，改用 appear 模式。

## 3. appear 模式（后备）

适用于 native 模式在某些 WPS/Office 版本上不按预期分步的情况。

- 视频仍由 `add_movie` 创建，但注入前必须删除 `p:video` 媒体节点：它们本身就是单击停点，与下面的单击组叠加会让每一步需要按两次空格。删除后由注入的 mediacall / `playFrom(0.0)` 组负责播放。
- 在根 `p:cTn` 的 `p:childTnLst` 中注入 `p:seq`：

```xml
<p:seq concurrent="1" nextAc="seek">
  <p:cTn id="100" dur="indefinite" nodeType="mainSeq">
    <p:childTnLst>
      <!-- 每个步骤一个单击组 -->
      <p:par>
        <p:cTn id="101" fill="hold" nodeType="afterEffect"> <!-- 首组自动；其余 clickEffect -->
          <p:stCondLst><p:cond delay="0"/></p:stCondLst>   <!-- 首组 0；其余 indefinite -->
          <p:childTnLst>
            <p:par>
              <p:cTn id="102" fill="hold">
                <p:stCondLst><p:cond delay="0"/></p:stCondLst>
                <p:childTnLst>
                  <!-- Appear：style.visibility = visible -->
                  <p:par>
                    <p:cTn id="103" presetID="1" presetClass="entr" presetSubtype="0"
                           fill="hold" nodeType="withEffect">
                      <p:childTnLst>
                        <p:set>
                          <p:cBhvr>
                            <p:cTn id="104" dur="1" fill="hold"/>
                            <p:tgtEl><p:spTgt spid="2"/></p:tgtEl>
                            <p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst>
                          </p:cBhvr>
                          <p:to><p:strVal val="visible"/></p:to>
                        </p:set>
                      </p:childTnLst>
                    </p:cTn>
                  </p:par>
                  <!-- 播放：playFrom(0.0) -->
                  <p:par>
                    <p:cTn id="105" presetID="1" presetClass="mediacall" fill="hold" nodeType="withEffect">
                      <p:childTnLst>
                        <p:cmd type="call" cmd="playFrom(0.0)">
                          <p:cBhvr>
                            <p:cTn id="106" dur="1" fill="hold"/>
                            <p:tgtEl><p:spTgt spid="2"/></p:tgtEl>
                          </p:cBhvr>
                        </p:cmd>
                      </p:childTnLst>
                    </p:cTn>
                  </p:par>
                </p:childTnLst>
              </p:cTn>
            </p:par>
          </p:childTnLst>
        </p:cTn>
      </p:par>
    </p:childTnLst>
  </p:cTn>
  <p:prevCondLst><p:cond evt="onPrev" delay="0"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:prevCondLst>
  <p:nextCondLst><p:cond evt="onNext" delay="0"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:nextCondLst>
</p:seq>
```

并追加 `p:bldLst`，每个视频形状一个 `p:bldP`。

首组 `afterEffect + delay="0"` 表示进页自动播放；其余组 `clickEffect + delay="indefinite"` 表示按空格触发。`--no-first-auto` 时全部为单击。

## 4. 结构校验

`verify_pptx.py` 应检查：

1. 页数 = 源页数 + 1。
2. 新页索引正确（1-based）。
3. 源 PPTX 中所有 `ppt/slides/slide*.xml` 的 C14N 哈希与新文件一致（原页语义未变）。
4. 目标页 `a:videoFile` 数量 = 步骤数。
5. native：`p:video` 媒体节点数 = 步骤数；首节点 `delay="0"`，其余 `indefinite`（或 `--no-first-auto` 时全部 `indefinite`）；且不出现注入的 `mainSeq`。
6. appear：`mainSeq` 的直接单击组数 = 步骤数；首组 `afterEffect`+`delay="0"`，其余 `clickEffect`+`indefinite`；`playFrom(0.0)` 与 `style.visibility` 各 = 步骤数；`p:video` 媒体节点数 = 0（否则与单击组叠加成双倍停点）。
7. 源稿哈希未变；`--out` 不得等于 `--source`。

## 5. 常见失败

| 现象 | 原因 | 修复 |
|------|------|------|
| 所有海报同时可见 / 黑块遮住第一段 | 第 2..N 段用了真实海报 | 改用透明海报 |
| 空格直接翻页、不播视频 | 没有点击停点，或模式写错 | native 检查 `p:video` 节点；appear 检查 `p:seq` |
| 首段不自动播放 | 首节点仍为 `indefinite` | 把第 1 个媒体节点 `delay` 改为 `0` |
| 多出一次空格才翻页 | appear 模式残留裸 `p:video` 节点 | 人工验证；必要时删除裸节点后重测 |
| 原页面被改动 | 直接保存到源文件 | 拒绝 `--out == --source`；另存新文件 |
| 新页位置错 | 混淆 0-based/1-based | `--after 12` 表示插在第 12 页之后，成为第 13 页 |
| 媒体被去重 | 两个步骤用了同一个文件 | 每段必须是不同的 MP4 |
