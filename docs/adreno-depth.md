# Adreno 540 深度格式问题：完整排查记录与未来指导

> 适用环境：Windows Unity 游戏（SPECIMEN: HIDE & SEEK / 标本躲猫猫 v1.0.0，Unity 6000.3.2f1）
> 运行栈：Wine + Box64 + DXVK-Sarek v1.12.0 → Vulkan → Adreno 540（msm8998 / a5x，Android Vulkan HAL）
> 显示：`DISPLAY=:1`（Termux:X11 屏幕）
> 核心修复文件：`~/proton11/.build/vulkan_adreno_icd.so`（Vulkan ICD 桥接 shim）

---

## 一、背景与原理

### 什么是「深度（depth）」
3D 画面里每个像素需要记录它离摄像机「有多远」，这张「距离图」叫**深度缓冲（depth buffer / z-buffer）**。
GPU 画每个像素时先比深度：更近的覆盖更远的，从而正确表现遮挡关系。
深度缓冲用的「像素格式」决定深度值怎么存、精度多高。常见格式：

| 格式 | Vulkan 枚举值 | 说明 |
|------|--------------|------|
| `VK_FORMAT_D16_UNORM` | 124 | 16 位深度 |
| `VK_FORMAT_X8_D24_UNORM_PACK32` | 125 | 24 位深度（无模板） |
| `VK_FORMAT_D24_UNORM_S8_UINT` | **129** | 24 位深度 + 8 位模板 |
| `VK_FORMAT_D32_SFLOAT` | 126 | 32 位浮点深度（无模板） |
| `VK_FORMAT_D32_SFLOAT_S8_UINT` | **130** | 32 位浮点深度 + 8 位模板 |

### 这套环境为什么需要 ICD shim
Adreno 540 的 Vulkan 驱动 `/vendor/lib64/hw/vulkan.msm8998.so` 是 Android Vulkan HAL，只导出 C++ 修饰名
`qglinternal::vkGetInstanceProcAddr`，没有标准 ICD 入口 `vk_icdGetInstanceProcAddr`，Mesa loader 无法直接加载。
`vulkan_adreno_icd.so` 在构造函数里：
1. 链接 namespace（default↔sphal），放行 `/vendor` 依赖；
2. `dlopen` Adreno HAL；
3. 导出标准 C 符号 `vk_icdGetInstanceProcAddr`，转调 HAL；
使 Mesa loader 能把它当普通 ICD 加载，从而枚举 Adreno 真实能力。

**关键事实**：shim 同时承担了「能力修补」职责——因为 Adreno 540 的 HAL 会对某些格式/特性**漏报或错报能力位**。

---

## 二、问题演进时间线

### 阶段 1：黑屏，只有 UI 文字
- 现象：文字/UI 正常，游戏画面区域全黑。
- 诊断：场景 RenderTexture 的深度缓冲创建失败 → 场景几何无法渲染 → 全黑。
- 方向：先修深度格式能力位。

### 阶段 2：黑屏 → 灰屏（UI 文字仍在，有声音）
- 修复深度能力位后，场景几何能渲染了，但「本该有图像的地方灰屏」。
- 误判：一度怀疑是 BC 压缩贴图（BC1–BC7 = 枚举 131–146）无法解码。
- 反证：抓 `vkCreateImage` 成功日志，发现游戏纹理 **95% 是 `fmt=37` (R8G8B8A8_UNORM 未压缩)**，BC 仅 11 张。
  → BC 解码不是灰屏主因，推翻该假设。

### 阶段 3：定位真正的元凶——RenderTexture 创建失败
- Unity `Player.log` 关键统计：
  - `RenderTexture.Create failed`：**5594 次**
  - `Failed to set the active render target`：**788 次**
- shim 日志关键发现：
  - `vkCreateImage FAILED`：**0 次**（纹理其实都创建成功了）
  - `vkGetPhysicalDeviceImageFormatProperties` 返回 `VK_ERROR_FORMAT_NOT_SUPPORTED (-11)`：**252 次**
  - 其中 **24 次是深度模板格式 `fmt=130`（D32_SFLOAT_S8_UINT）**，usage=0x4（SAMPLED）探测失败。
- 机理（决定性）：DXVK 的 `D3D11CommonTexture::CheckImageSupport` 在**启动期**对每种 DXGI 格式用不同 usage 各探测一次：
  - `usage=DEPTH_STENCIL_ATTACHMENT (0x20)` → 能否当深度附件
  - `usage=SAMPLED (0x4)` → 深度纹理能否当着色器资源采样
  - **任一 usage 返回 -11，DXVK 就将该格式整体标记为「不支持」并缓存**。
  - 之后 Unity 每次建 RenderTexture（深度缓冲）都直接从缓存返回失败，**根本走不到 `vkCreateImage`**——这正是「5594 次失败 vs 0 次 vkCreateImage 失败」的悖论的来源。

### 阶段 4：修复能力查询（灰→可见，但开始闪）
- 在 `shim_vkGetPhysicalDeviceImageFormatProperties`（iffp）里放宽深度格式：
  之前要求 `usage` 含 `DEPTH_STENCIL_ATTACHMENT` 才放宽，但 `usage=0x4 (SAMPLED)` 的探测仍返回 -11。
  改为：对深度/模板格式，只要 usage 落在
  `DEPTH_STENCIL_ATTACHMENT | SAMPLED | INPUT_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST`
  之内就返回 `VK_SUCCESS`（保守的 `VkImageFormatProperties`）。
- 结果：`RenderTexture.Create failed` **5594 → 0**，`active render target` **788 → 0**。场景可见，但——

### 阶段 5：纹理闪烁（用户描述「贴图部分一直在闪，不是垂直同步问题」）
- 现象：画面显示出来了，但带贴图的表面持续闪烁（z-fighting 式抖动）。
- 诊断：
  - HAL 在 `ImageFormatProperties` 里对 D32S8 一律返回 `-11`，而 `D24S8 (129)` 是**真正受支持**的（同查询返回成功）。
  - 阶段 4 的修复只是「骗 DXVK D32S8 可用」，于是 DXVK 真去创建了 D32S8 深度图；但硬件**并不能正确渲染 D32S8**，深度缓冲实际是坏的 → 带贴图表面的深度测试错乱 → 闪烁。
  - 注意：之前日志里 `vkCreateImage OK fmt=130` 是**假象**——Vulkan 的 `vkCreateImage` 并不严格按格式能力校验，权威判断是 `ImageFormatProperties` 查询；「创建成功」≠「能用」。

### 阶段 6：透明替换 D32S8 → D24S8（闪烁消失）
- 修复方式：在 shim 中把 `VK_FORMAT_D32_SFLOAT_S8_UINT` **透明替换**成硬件真正支持的 `VK_FORMAT_D24_UNORM_S8_UINT`，且**三处必须一致替换**，否则 Vulkan 会因 image/view/renderpass 格式不匹配而报错：
  - `vkCreateImage`
  - `vkCreateImageView`
  - `vkCreateRenderPass` / `vkCreateRenderPass2` / `vkCreateRenderPass2KHR`
- 结果：闪烁消失，`RenderTexture.Create failed` 仍为 0，无崩溃。

---

## 三、关键发现（经验证据）

### 1. Adreno 540 HAL 对深度格式的真实能力（RAW 模式实测）
在 shim 中加 `VK_TEST_RAW=1` 开关（禁用一切修补/替换），直连硬件查询，结果：

| 格式 | FormatProperties.optimal | DEPTH_ATTACHMENT | ImageFormatProperties(深度附件) |
|------|------------------------|------------------|-------------------------------|
| **D32_SFLOAT_S8_UINT (130)** | `0x0`（零特性位） | **no** | **VK_ERROR_FORMAT_NOT_SUPPORTED** |
| **D24_UNORM_S8_UINT (129)** | `0x1d601` | **YES** | **VK_SUCCESS** |

结论：**Adreno 540 唯一不支持的深度格式就是 D32S8**；D16 / D24X8 / D24S8 / D32F 都正常。

### 2. 功能渲染验证（depthtest.c）
画「近绿三角(深度 0.3) + 远蓝三角(深度 0.7)」覆盖同一区域，深度测试 `LESS`，读回中心像素：

| 深度格式 | 中心像素 | 结论 |
|----------|----------|------|
| **D24S8 (129)** | `RGB=(0,255,0)` 绿 | 深度测试正确，远蓝被正确遮挡 ✅ |
| **D32S8 (130)** | **进程段错误 (SIGSEGV, exit 139)** | 驱动在绑定/使用 D32S8 深度图时直接崩溃 ❌ |

→ 硬件层面 D32S8 完全不可用，D32S8→D24S8 替换是**唯一正确做法**。

### 3. 为什么之前 BC 假设被推翻
游戏实际纹理格式统计（`vkCreateImage OK`）：
- `fmt=37` (R8G8B8A8_UNORM)：2623 张
- `fmt=9` (R8)：14 张；`fmt=43`：13 张；`fmt=134/138/98` 等少量；BC(131–146) 仅 11 张
→ 主画面贴图是未压缩 RGBA8，BC 解码不是灰屏原因。UI 文字能显示正是因为字体图集是未压缩的。

### 4. shim 当前生效的修补项（默认开启，游戏正常）
- `fmtp_fix_depth`：为深度/模板格式补齐能力位；`D32S8` 复制 `D24S8` 的真实能力。
- `iffp` / `iffp2`：放宽深度格式在 `SAMPLED/TRANSFER` 等 usage 下的 `-11`。
- `vkCreateImage/View/RenderPass(/2)`：D32S8→D24S8 透明替换（**命令层同步拦截 clear/copy/blit/resolve**，保持 D24S8 一致布局）。
- **BC 压缩格式补位（门控，默认关闭）**：对 BC1~BC7 补 `SAMPLED/TRANSFER/BLIT`。**仅 `VK_ICD_BC_FIX=1` 时生效**，目前只在 星白列车脚本里开启。
- **颜色格式 COLOR_ATTACHMENT 补位（门控，默认关闭）**：对「可采样但缺 COLOR_ATTACHMENT」的颜色格式补 `COLOR_ATTACHMENT+BLEND+TRANSFER+BLIT`。**仅 `VK_ICD_RTV_FIX=1` 时生效**，且只在 星白列车脚本里开启。
  ⚠️ BC 与颜色补位对 **星白列车** 必需（缺失→3D 黑屏有声音），但对 **Undertale / gal 等游戏反而会因谎报能力位导致黑屏**。因此两者都**绝不能无条件开启**——默认全关，只在需要的脚本里打对应开关。
- `VK_TEST_RAW=1`（仅测试用）：关闭以上所有修补 + 关闭「低位 dmabuf 重映射」。
- `VK_ICD_MAP_LOW=0`：关闭低位 dmabuf 重映射（原诊断开关）。
- `VK_ICD_RTV_FIX=1` / `VK_ICD_BC_FIX=1`：**只在需要补位的游戏脚本里设**（目前仅 `claunch-mashiro-adreno.sh`）。

> ⚠️ **历史教训（2026-08-19，四次踩坑，务必记牢）**：
> - 标本躲猫猫 3D 黑屏根因是 **DXVK shader cache 损坏**（见「急救步骤 0」），与格式能力无关；清 cache 即解决。
> - 星白列车（星空列车与白的旅行）黑屏**不是** cache 问题，而是缺 **BC 补位 + 颜色 COLOR_ATTACHMENT 补位**。两者都**门控**在开关后（默认关），仅星白脚本里开 `VK_ICD_BC_FIX=1` + `VK_ICD_RTV_FIX=1`。
> - **BC / 颜色补位都绝不能无条件开启**：曾把它们改成无条件后，Undertale / gal 等原本正常的游戏**集体黑屏**（谎报能力位所致）。正确基线 = 两个补位都默认关闭，只在该开的游戏脚本里打对应开关。
> - `vulkan_gpu.so`（`LD_PRELOAD` 加载的伞兵）必须与 `vulkan_adreno_icd.so` 保持同一份二进制，每次 Rust 重建后 `cp` 同步。

---

## 四、诊断流程（可复用的标准动作）

当某个 Windows 游戏在本环境出现**灰屏 / 黑屏 / 闪烁 / 穿模 / 缺贴图**时，按此顺序排查：

### 步骤 0（最优先！）：先排除 DXVK shader cache 损坏 —— 黑屏却无声、无报错
> 经验铁律：**「3D 全黑 + UI 文字正常 + 有声音 + 无任何游戏/驱动报错」组合，先怀疑 DXVK cache，而不是 shim 格式修补。**
> shim 的格式修补若真有缺口，通常会伴随 `vkCreateImage FAILED` / `iffp r=-11` 日志或 Unity `RenderTexture.Create failed`；
> **cache 损坏则完全静默**——DXVK 加载了坏 pipeline 缓存，3D 管线静默失败，渲染出来就是一片黑，但游戏逻辑/UI 照常。

排查动作（极低成本，先试这个再动代码）：
1. 关掉游戏（必要时 `pgrep -f SPECIMEN | xargs kill -9`，**不要用 `pkill -f` 误杀本 shell**）。
2. 备份并删除该游戏的 DXVK cache：
   ```
   DXVK_CACHE="$HOME/proton11/p11prefix/drive_d/users/xuser/AppData/Local/dxvk/<游戏名>.dxvk-cache"
   cp -f "$DXVK_CACHE" "$DXVK_CACHE.bak"   # 先备份，可恢复
   rm -f "$DXVK_CACHE"
   ```
   - 文件名形如 `SPECIMEN_HIDE_SEEK.dxvk-cache` / `星空列车与白的旅行.dxvk-cache` / `nine_kokoiro.dxvk-cache` 等，
     命名规则 = Unity 产品名（Player.log 同目录名）。不确定时 `ls -lt ~/.../Local/dxvk/` 看最近改动的那一个。
3. 重开游戏。DXVK 会重建一份干净的 cache（大小通常从「异常大/异常小」回到几 KB～十几 KB）。
   - **若重建后 3D 立刻恢复 → 根因就是坏 cache，与 shim 无关**，不必再改格式修补。
   - 备份的 `.bak` 可留作证据，确认恢复后可删。

> 真实案例（2026-08-19）：标本躲猫猫 3D 全黑无轮廓、UI/声音正常、无报错。
> 一度怀疑 BC 压缩贴图 / 颜色 COLOR_ATTACHMENT 能力缺失并加了补位，实测无效；
> 最终删除 `SPECIMEN_HIDE_SEEK.dxvk-cache`（当时 12552 字节、已损坏）后，C 版与 Rust 版 ICD **同时**恢复 3D 画面。
> 结论：BC / 颜色补位都不是根因，纯属误打误撞之外的负优化，已全部回退。

### 步骤 A：抓两份日志
启动游戏前清空日志，运行 ~40 秒后分析：
```
rm -f ~/proton11/.build/adreno-specimen.log
# 启动：bash 标本躲猫猫.sh  (后台)
# 等 40s
```
- **Unity 日志**：`~/proton11/p11prefix/drive_d/users/xuser/AppData/LocalLow/Rain_Without_Sound/SPECIMEN_HIDE_SEEK/Player.log`
- **shim 日志**：`~/proton11/.build/adreno-specimen.log`

### 步骤 B：快速统计
```
# RenderTexture / 渲染目标失败计数
grep -ac "RenderTexture.Create failed" <Player.log>
grep -ac "active render target"        <Player.log>

# 实际图像创建是否失败（0 说明问题在「能力查询」而非「创建」）
grep -ac "vkCreateImage FAILED" <adreno-specimen.log>

# 哪些格式被 HAL 判为不支持
grep -a "fmtp UNSUPPORTED" <adreno-specimen.log>

# 图像格式能力查询失败（-11）按格式归类
grep -a -B1 "iffp real r=-11" <adreno-specimen.log> | grep "iffp called" \
  | grep -oE "fmt=[0-9]+ .* usage=0x[0-9a-f]+" | sort | uniq -c

# 实际纹理用的格式分布（判断是不是 BC 压缩问题）
grep -a "vkCreateImage OK" <adreno-specimen.log> | grep -oE "fmt=[0-9]+" \
  | sort | uniq -c | sort -rn
```

### 步骤 C：判断是「能力缺失」还是「创建失败」
- `vkCreateImage FAILED=0` 但 `RenderTexture.Create failed` 很多 → **能力查询层把格式判死了**（如本例 D32S8）。
  - 看 `iffp real r=-11` 的格式/usage，确认是哪个格式 + 哪种 usage 触发。
  - 对照 Vulkan 枚举值表，判断是深度 / BC / 颜色格式。
- `vkCreateImage FAILED` 很多 → 真实创建问题，看具体 `fmt=` 与 `r=`。

### 步骤 D：用测试程序直连硬件确认（最硬的证据）
`VK_TEST_RAW=1` 下运行，绕过 shim 修补，看硬件真实能力：
```
export LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib
export VK_ICD_FILENAMES=$HOME/proton11/.build/vulkan_adreno_icd.json
export VK_TEST_RAW=1
./qfmt          # 打印各深度格式的原始 FormatProperties / ImageFormatProperties
./depthtest 1   # D24S8 真实深度测试渲染（应得绿=正常）
./depthtest 0   # D32S8 真实深度测试渲染（本硬件会段错误=不可用）
```
- 测试程序位置：`/data/data/com.termux/files/home/depthtest.c`、`qfmt.c`、对应的 `*.spv`、编译产物 `depthtest`、`qfmt`。
- 编译：`gcc <file>.c -o <file> -lvulkan -I/data/data/com.termux/files/usr/include`（着色器先用 `glslangValidator -V` 编译成 `.spv`）。

### 步骤 E：施加修复（在 shim 里）
> 先确认已经过「步骤 0（DXVK cache）」与「步骤 C（能力 vs 创建）」定位，再动代码。
确认坏格式后，照下面任一类方式补：
- **深度/模板**：在 `fmtp_fix_depth` 补能力位；在 `iffp`/`iffp2` 放宽 `-11`；必要时在 `vkCreateImage/View/RenderPass(/2)` 做**等价格式替换**（必须三处一致，命令层 clear/copy/blit/resolve 也要同步）。
- **BC 压缩**：`fmtp_fix_depth` 的 BC 分支补 `SAMPLED/TRANSFER/BLIT`；`fmtp_fix_depth_pnext` 补 `2KHR` 的 `COLOR_ATTACHMENT|SAMPLED`。`iffp`/`iffp2` 放宽 BC 的 `-11`。**注意 BC 补位对星白列车等游戏是必需的，不要为「标本只需清 cache」而整体回退 BC 补位**（历史坑：曾因此让星白回归黑屏）。
- Rust 版改完：在 `icd-rs/` 下 `bash build.sh`（clippy 零警告 + 构建），自动安装到 `~/proton11/.build/vulkan_adreno_icd.so`。

---

## 五、对未来的指导

### 1. 本问题（D32S8）已永久修复，不会复发
- 替换逻辑写进 shim，每次启动都生效；且深度格式全家桶中**只有 D32S8 一种坏**，已覆盖。
- 唯一理论例外：某游戏硬性要求 32 位浮点深度精度（超大型开放世界用对数深度才可能需要），D24S8 的 24 位可能轻微 z-fighting。但 99% 的游戏 D24S8 完全够用。

### 2. 「这类问题」（老 Adreno 的能力缺口）可能在别的游戏再现
Adreno 540（a5x）是很老的 Vulkan 实现，HAL 会对不少格式/特性漏报或错报能力位。已确认并修复：
- 深度格式 D32S8（已修，透明替换为 D24S8）
- BC 压缩格式 BC1–BC7（已补 `SAMPLED/TRANSFER/BLIT`，**星白列车等 2D 游戏必需**）

> 颜色 COLOR_ATTACHMENT 补位（`VK_ICD_RTV_FIX`）仍维持移除：对标本无效（真因是 DXVK cache）、对星白无必要。

**未来别的游戏**可能遇到：没覆盖到的某个格式、某个特性位（如 `geometryShader`、`logicOp`、`sampleRateShading`）、某种 MSAA 采样数、某个扩展等。

### 3. 但已有成熟打法，不必怕
- 诊断方法固定：shim 日志 + `VK_TEST_RAW=1` 直连查询 + `qfmt.c` / `depthtest.c` 两个测试程序。
- 修复套路固定：在 shim 中「如实暴露 → 必要时替换为硬件支持的等价格式」。
- 建议**保留** `VK_TEST_RAW` 开关、低位 dmabuf 的 `VK_ICD_MAP_LOW` 开关、以及 `qfmt.c`/`depthtest.c` 测试程序，作为本环境的常驻诊断工具。

### 4. 先兆识别（出现这些现象，先想到「Vulkan 能力被 Adreno 漏报」）
- 画面灰/黑，但 UI 文字正常 → 大概率深度或 RenderTexture 颜色格式能力缺失。
- 画面可见但带贴图表面闪烁/抖动 → 深度格式「假支持真坏」，需做等价格式替换。
- 整屏撕裂（随窗口移动/滚动画面）→ 才是垂直同步/呈现模式问题（`VK_PRESENT_MODE_IMMEDIATE`），与深度无关（本例用户明确排除了这种）。

### 5. 不要过度修补
- 只放宽「确实被 HAL 误判、且硬件真能用的格式」。例如 D32S8 我们**没有**谎称它支持，而是替换为 D24S8——因为硬件确实渲不了 D32S8（否则会像阶段 5 那样闪烁甚至崩溃）。
- 替换格式时务必 image、view、renderpass 三处一致，否则 Vulkan 报格式不匹配错误。

---

## 六、关键文件清单

| 文件 | 作用 |
|------|------|
| `~/proton11/.build/vulkan_adreno_icd.c` | C shim 参考源码（与 Rust 版逻辑一致；仅作对照） |
| `~/proton11/.build/vulkan_adreno_icd.so` | Rust 版编译产物，ICD JSON 实际加载对象（星白以外的游戏走此路径） |
| `~/proton11/.build/vulkan_gpu.so` | **必须与 `vulkan_adreno_icd.so` 保持同一份二进制**——`*-adreno.sh` 脚本经 `LD_PRELOAD` 加载它，且它也导出 `vk_icdGetInstanceProcAddr`。每次 Rust 重建后必须 `cp vulkan_adreno_icd.so vulkan_gpu.so`，否则星白等游戏仍跑旧 C 二进制、所有改动失效 |
| `~/proton11/.build/vulkan_adreno_icd.json` | ICD 描述，指向 `vulkan_adreno_icd.so` |
| `标本躲猫猫.sh` | 游戏启动脚本（`/data/data/com.termux/files/home/basement/loveai/SPECIMEN_HIDE_SEEK_v1.0.0/`） |
| `~/proton11/p11prefix/drive_d/users/xuser/AppData/LocalLow/Rain_Without_Sound/SPECIMEN_HIDE_SEEK/Player.log` | Unity 日志 |
| `~/proton11/.build/adreno-specimen.log` | shim 诊断日志 |
| `/data/data/com.termux/files/home/depthtest.c` + `depthtest.vert/frag.spv` | 深度格式功能渲染测试 |
| `/data/data/com.termux/files/home/qfmt.c` | 深度格式原始能力查询测试 |

---

## 七、一句话总结
Adreno 540 的 HAL 把唯一一种 32 位浮点深度格式 `D32_SFLOAT_S8_UINT` 报成「完全不支持」，
而我们最初只是「骗 DXVK 它可用」→ 场景出来了但深度是坏的、贴图闪烁；
最终正确做法是把它**透明替换成硬件真正支持的 `D24_UNORM_S8_UINT`**（image/view/renderpass 三处一致）。
证据由 `qfmt.c` / `depthtest.c` 两级测试坐实：D24S8 渲染正确，D32S8 直接让驱动崩溃。
本问题已永久修复；老 Adreno 的其他能力缺口若在未来别的游戏出现，用同样的「抓日志 → RAW 查询 → 等价替换」流程即可。
