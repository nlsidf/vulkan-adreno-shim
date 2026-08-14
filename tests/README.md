# tests/ — Vulkan 格式能力诊断测试

本目录下的 `depthtest.c` 与 `qfmt.c` 用于**绕过 shim 的格式修补、直连 Adreno 540 真实 HAL**，
验证某个 Vulkan 格式（尤其是深度格式）到底是否被硬件真正支持。

## 前置环境
- Termux 下已有 Vulkan loader：`/data/data/com.termux/files/usr/lib/libvulkan.so`
- 着色器编译工具：`glslangValidator`（已随系统安装）
- shim 二进制与 ICD 清单：`~/proton11/.build/vulkan_adreno_icd.so` + `vulkan_adreno_icd.json`

## 编译
```bash
# 着色器 -> SPIR-V
glslangValidator -V depthtest.vert -o depthtest.vert.spv
glslangValidator -V depthtest.frag -o depthtest.frag.spv

# 测试程序
gcc qfmt.c      -o qfmt      -lvulkan -I/data/data/com.termux/files/usr/include
gcc depthtest.c -o depthtest -lvulkan -I/data/data/com.termux/files/usr/include
```

## 运行（必须开 VK_TEST_RAW=1，否则 shim 会修补/替换格式，看不到真实硬件行为）
```bash
export LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib
export VK_ICD_FILENAMES=$HOME/proton11/.build/vulkan_adreno_icd.json
export VK_TEST_RAW=1

./qfmt            # 打印各深度格式的原始 FormatProperties / ImageFormatProperties
./depthtest 1     # D24_UNORM_S8_UINT(129) 真实深度测试渲染，应得 RGB=(0,255,0) 绿=正常
./depthtest 0     # D32_SFLOAT_S8_UINT(130) 真实深度测试渲染，本硬件会段错误=不可用
```
> 说明：`depthtest` 接受可选参数 `0`(仅测 D32S8) / `1`(仅测 D24S8)，不传则两个都测（D32S8 崩溃会中断 D24S8）。

## 背景结论（详见 docs/adreno-depth.md）
Adreno 540 的 HAL 把 `D32_SFLOAT_S8_UINT`(130) 报成「完全不支持」（零特性位、
`ImageFormatProperties` 返回 `VK_ERROR_FORMAT_NOT_SUPPORTED`），而 `D24_UNORM_S8_UINT`(129)
是硬件真正支持的深度格式。因此 shim 在 `icd/vulkan_adreno_icd.c` 中把 D32S8 **透明替换**为 D24S8
（image / view / renderpass 三处一致替换），这是修好 SPECIMEN 等游戏「灰屏→可见但闪烁」的关键。

这两个测试程序的实证结果：D24S8 渲染中心像素为绿（深度正确），D32S8 直接让驱动段错误（不可用）。
