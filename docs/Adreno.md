# Adreno 540 Vulkan GPU 渲染指南 (Proton11 / Termux)

> 状态: **✅ 64 位游戏上屏成功 (2026-08-08)**
> 用户确认:"出现画面了！有声音能操作！"
> 本文件汇总: 架构、全部组件、三项关键修复、启动方法、验证证据、排查手册、已知限制与后续任务。

---

## 0. 一句话总结

在 Termux (Android, 无 proot) 里, 用 **box64 (x86_64→ARM64 动态翻译) + Proton11 (Wine) + DXVK-Sarek (D3D→Vulkan) + 自定义 Vulkan shim + Sky1 WSI 层** 让游戏走 **Adreno 540 真实 GPU** 渲染, 并通过 X11 (TigerVNC) 把画面呈现出来。此前"黑屏有声音能盲操作"的根因已全部修复。

---

## 1. 渲染链路架构

```
Windows 游戏 (.exe, 64 位)
  │  D3D11 API
  ▼
DXVK-Sarek v1.12.0          ← x86_64 DLL (box64 翻译), d3d11→Vulkan
  │  Vulkan 1.1/1.2 API
  ▼
winevulkan.dll / winevulkan.so  ← Wine 自带的 Vulkan loader 桥接
  │
  ▼
Mesa Vulkan loader (libvulkan.so, 宿主原生 ARM64)
  │  VK_ICD_FILENAMES=vulkan_adreno_icd.json
  ▼
vulkan_adreno_icd.so (自定义 ICD shim) ← 本项目的桥接关键
  │  链接 namespace → dlopen Adreno HAL → 转发 + 拦截修正
  ▼
Adreno 540 Vulkan HAL (/vendor/lib64/hw/vulkan.msm8998.so, Android 驱动)
  │  渲染出图像 (Vulkan 1.1.87, 无 WSI)
  ▼
Sky1 WSI 层 (VK_LAYER_window_system_integration, 隐式 layer)
  │  实现 VK_KHR_surface/swapchain, 把图像拷贝到 X11
  ▼
X11 SHM 呈现 (MIT-SHM 共享内存, CPU 拷贝) → Xvnc :1 → 屏幕
```

**三条平行启动路径** (游戏类型不同):
| 路径 | 渲染后端 | 用途 |
|------|---------|------|
| **Adreno Vulkan (本文)** | Adreno 540 HAL | 64 位 Unity 游戏 (星空列车) ✅ |
| lavapipe (软件) | Mesa lavapipe CPU | 任何游戏验证管线 (9-nine 也通) |
| VirGL / wined3d | Adreno EGL 转发 | OpenGL 路径, 备选 |

---

## 2. 组件清单与文件位置

| 组件 | 路径 | 说明 |
|------|------|------|
| box64 | `~/proton11/box64/build/box64` | x86_64→ARM64 动态重编译器 |
| Wine/Proton | `~/proton11/xaw64_wine/proton-11/bin/wine` | 64 位 wine (proot 版) |
| DXVK-Sarek | `~/proton11/dxvk-sarek-1.12.0/` | x64→system32, x32→syswow64 |
| **ICD shim** | `~/proton11/.build/vulkan_adreno_icd.so` | Adreno HAL 桥接 + 拦截修正 (编译自 `.c`) |
| **ICD 清单** | `~/proton11/.build/vulkan_adreno_icd.json` | 让 Mesa loader 加载 shim |
| **namespace 库** | `~/proton11/.build/vulkan_gpu.so` | 链接 default↔sphal, 放行 /vendor 依赖 |
| **WSI 层** | `~/proton11/.build/vulkan-wsi-layer/` | Sky1 WSI 层源码 (Arm/ginkage fork) |
| WSI 层 .so | `~/proton11/.build/vulkan-wsi-layer/build/libVkLayer_window_system_integration.so` | 构建产物 |
| 层安装位置 | `/data/data/com.termux/files/usr/share/vulkan/implicit_layer.d/` | `.so` + `.json` 同目录 |
| fake machine-id | `~/fake_machineid.so` + `~/.fake_machine_id` | proot 只读 /etc/machine-id 劫持 |
| 音频 | `~/proton11/start-audio.sh` | PulseAudio → Android AAudio |
| **启动器 (Adreno)** | `~/proton11/claunch-skyrail-adreno.sh` | 64 位星空列车 |
| 启动器 (Adreno 32位) | `~/proton11/claunch-9nine-adreno.sh` | 9-nine (未通, 见 §9) |

---

## 3. 三大组件的工作原理

### 3.1 vulkan_gpu.so — Android namespace 绕过

Android 用 linker namespace 隔离 `/vendor` 库, Termux 进程默认看不到 Adreno 驱动。该库在**构造函数**里调用:

```
__loader_android_get_exported_namespace("sphal")  → 取得 sphal namespace
__loader_android_link_namespaces(default, sphal, 库列表) → 放行依赖
```

放行列表: `vulkan.msm8998.so`, `libEGL_adreno.so`, `libGLESv2_adreno.so`, `libGLESv1_CM_adreno.so`, `libq3dtools_adreno.so`, `libadreno_utils.so`, `libgsl.so`, `libllvm-glnext.so`, `libcutils.so`, `libutils.so`, `libhardware.so`, `libnativewindow.so`, `libvulkan.so`, `libvkjson.so`, `libsync.so`。

编译: `gcc -shared -fPIC -o vulkan_gpu.so vulkan_gpu.c -ldl` (源码自带编译注释)
启用: `LD_PRELOAD=...:vulkan_gpu.so` (或由 ICD shim 自身内置同名逻辑)。

### 3.2 vulkan_adreno_icd.so — Adreno HAL 桥接 shim

**问题**: Adreno 驱动 `vulkan.msm8998.so` 是 Android HAL, 只导出 C++ 修饰名 `_ZN11qglinternal21vkGetInstanceProcAddrEP12VkInstance_TPKc`, **没有**标准 ICD 入口 `vk_icdGetInstanceProcAddr`, Mesa loader 无法直接加载。

**解决**: shim 在构造函数里 `dlopen("/vendor/lib64/hw/vulkan.msm8998.so")` 取 `qglinternal::vkGetInstanceProcAddr`, 然后导出标准 ICD 入口 `vk_icdGetInstanceProcAddr` / `vk_icdGetPhysicalDeviceProcAddr`, 把 Mesa loader 的调用**转发**给真实 HAL, 并拦截 4 个函数做修正 (见 §4):

| 拦截函数 | 修正内容 |
|---------|---------|
| `vkCreateInstance` | 记录真实实例句柄 (多实例安全) |
| `vkEnumeratePhysicalDevices` | 记录 PD→实例 归属 (多实例安全) |
| `vkCreateDevice` | 剥离 win32 平台扩展 + 清 pNext 链 (DXVK-Sarek 兼容) |
| `vkGetPhysicalDeviceImageFormatProperties` | **深度格式放宽** (见 §4.2) |

ICD 清单 `vulkan_adreno_icd.json`:
```json
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so",
        "api_version": "1.0.0"
    }
}
```

编译: `gcc -shared -fPIC -o vulkan_adreno_icd.so vulkan_adreno_icd.c -ldl`
启用: `VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json`

### 3.3 Sky1 WSI 层 — X11 呈现

fork 自 Arm 的 Vulkan WSI layer (`vulkan-wsi-layer`, 即 ginkage 项目), 实现了 `VK_KHR_surface` / `VK_KHR_swapchain` 等扩展, 把 Vulkan swapchain 图像呈现到 X11。**因为 Adreno HAL 完全没有 WSI (只有 Android ANativeWindow), 这一层是必须的。**

- 隐式 layer → 所有 Vulkan 应用自动加载, 无需配置
- 安装: 把 `build/libVkLayer_window_system_integration.so` 和 `VkLayer_window_system_integration.json` 一起放进 `/data/data/com.termux/files/usr/share/vulkan/implicit_layer.d/`
- 构建配置: `BUILD_WSI_X11=ON`, `BUILD_WSI_DISPLAY=OFF`, `BUILD_WSI_WAYLAND=OFF`, `BUILD_WSI_HEADLESS=OFF`, `ENABLE_ARM_NEON=ON`, `CMAKE_BUILD_TYPE=RelWithDebInfo`, Termux 自带 gcc/g++ (`/data/data/com.termux/files/usr/bin/cc` / `c++`)
- 重构建: `cd ~/proton11/.build/vulkan-wsi-layer/build && make -j4`, 然后把新 `.so` 拷回 implicit_layer.d

---

## 4. 三项关键修复 (缺一不可, 按顺序叠加)

### 4.1 独立 xcb 连接 → 消灭 XIO fatal IO error 9

**文件**: `wsi/x11/surface.cpp` (`surface::make_surface`)

**问题**: WSI 层从 winevulkan/winex11 拿到的是 **x86_64 libxcb 在 box64 下创建的共享连接**。本层是原生 ARM64, 两个 libxcb 实例操作同一 socket fd, 结构体布局不一致会互相破坏读缓冲/序列追踪。实测: 共享连接在第一次 `xcb_shm_attach_fd` (SCM_RIGHTS) 就死, 连带 wine 的 X 客户端一起挂 → `XIO: fatal IO error 9 (Bad file descriptor)`。

**修复**: 读到 `DISPLAY` 就用**原生 ARM64 libxcb 开自己的连接**, 并验证窗口在该连接上可查询 (geometry roundtrip)。X window ID 是 server 全局的, wine 创建的窗口在本层连接上同样有效。自己的连接失败时才回退到传入连接。

关键行为:
- 成功时日志: `Surface 0x... using its own xcb connection to :1`
- 析构时**只断开自己的连接**, 绝不碰 wine 共享连接 (注释明确警告)
- swapchain 的 `m_connection` 取自 `wsi_surface.get_connection()` → presenter 用的就是这条独立连接

### 4.2 深度格式 shim 放宽 → 消灭 "Cannot create texture"

**文件**: `vulkan_adreno_icd.c` (`shim_vkGetPhysicalDeviceImageFormatProperties`)

**问题**: Adreno 540 HAL 对**所有**深度/模板格式的 `vkGetPhysicalDeviceImageFormatProperties` 一律返回 `VK_ERROR_FORMAT_NOT_SUPPORTED (-11)`, 连 D16_UNORM 都失败 (原生探针 `imgfmt_probe.c` 实测)。但 `vkCreateImage` 对这些格式**全部成功** — HAL 内部不一致。DXVK 的 `CheckImageSupport` (d3d11_texture.cpp:453) 依赖该查询, 误判为不支持 → 抛 `Cannot create texture` (d3d11_texture.cpp:204) → Unity 游戏崩溃。

**修复**: 拦截该函数。当真实调用返回 -11 且:
- `usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT (0x20)` 为真
- 格式属深度家族 (白名单: `D16_UNORM(124)` `X8_D24_UNORM_PACK32(125)` `D32_SFLOAT(126)` `S8_UINT(127)` `D16_UNORM_S8_UINT(128)` `D24_UNORM_S8_UINT(129)` `D32_SFLOAT_S8_UINT(130)` + typeless `37/38/39`)

→ 返回 `VK_SUCCESS` + 保守能力值 (8192x8192, mip=15, layers=256, samples=1/2/4)。日志特征: `[VK_ICD] iffp 深度格式放宽: fmt=130 tiling=0 usage=0x23 -> VK_SUCCESS` (fmt=130 = D32_SFLOAT_S8_UINT, DXVK 把 DXGI 格式 19 系列全映射到它)。

> 注意: 深度家族用**数值字面量** — Termux 的 vulkan_core.h 被修剪, 没定义这几个枚举名。

### 4.3 libxcb 扩展查询杀连接 → 消灭数百条 xcb_flush 失败 (最终根因)

**文件**: `wsi/x11/dri3_presenter.cpp` (`query_extension_present` + `query_dri3_present`)

**问题**: swapchain 在回退 SHM 前调用 `dri3_presenter::is_available` → `query_dri3_present` → 无条件调用 `xcb_dri3_query_version()` / `xcb_present_query_version()`。而 **libxcb 的生成函数内部通过 `_xcb_conn_ext` 解析扩展主 opcode; 扩展不存在时直接 `_xcb_conn_shutdown(XCB_CONN_CLOSED_EXT_NOTSUPPORTED)` 杀掉整条连接** — 不是返回 NULL, 而是**杀死连接**。Xvnc 上 DRI3/Present/XSync 全部缺席 → 连接死 → 后续所有 `create_gc` / `attach_fd` / `present` / flush 全部在死连接上执行 → 数百条 `xcb_flush failed: errno=11 has_error=2`。

**关键认知**: `xcb_connection_has_error()==2` 就是 `XCB_CONN_CLOSED_EXT_NOTSUPPORTED` (扩展不支持导致连接关闭), 不是 fd/网络问题。errno 只是被杀时的残留值, 表面证据极具误导性。

**修复**: 新增静态 helper `query_extension_present()`, 用**原始核心请求** `xcb_query_extension` (永远不会杀连接, 返回 `present=0` 即提前退出) 前置检查 DRI3 和 Present:

```cpp
static bool query_extension_present(xcb_connection_t *connection, const char *name)
{
   /* Raw core QueryExtension request.  Unlike the generated
    * xcb_<ext>_query_version() helpers this NEVER kills the connection:
    * the generated functions resolve the major opcode via _xcb_conn_ext,
    * which shuts the whole connection down with
    * XCB_CONN_CLOSED_EXT_NOTSUPPORTED (has_error=2) when the extension is
    * absent.  We need the connection to survive a negative answer so the
    * SHM fallback can still present. */
   xcb_query_extension_cookie_t cookie =
      xcb_query_extension(connection, static_cast<uint16_t>(strlen(name)), name);
   xcb_query_extension_reply_t *reply = xcb_query_extension_reply(connection, cookie, nullptr);
   if (!reply) return false;
   bool present = reply->present != 0;
   free(reply);
   return present;
}
```

`query_dri3_present` 现在**先查存在性再调生成函数**: DRI3 或 Present 任一缺失即返回 false → 安全回退 SHM。日志特征: `x11 swapchain: DRI3 not available` → `x11 swapchain: using SHM fallback presenter`, 连接保持健康, present 持续。

**证据链** (三个步进式原生测试, 编译为 aarch64):
| 工具 | 证明 |
|------|------|
| `ext_query_test` | SHM OK → DRI3 query_version reply=NULL **且 has_error=2** → 连接死 |
| `ext_query_test2` | 原始 `xcb_query_extension` 和 `xcb_get_extension_data` 都**不杀连接**, roundtrip 正常 |
| `ext_query_test3` | 决定性命中: `xcb_dri3_query_version` **调用本身**(reply 之前)就已 has_error=2 → 杀连接发生在编码阶段 |

---

## 5. 如何启动 (GPU 渲染)

### 5.1 前置条件

1. TigerVNC 在跑 (`:1`, 端口 5901): `export DISPLAY=:1`
2. PulseAudio 音频 (自动由启动器拉起): `~/proton11/start-audio.sh`
3. WSI 层已安装 (`.so` + `.json` 在 `/data/data/com.termux/files/usr/share/vulkan/implicit_layer.d/`)
4. box64 已构建: `~/proton11/box64/build/box64`

### 5.2 64 位 Unity 游戏 (星空列车) — 已验证

```bash
cd ~/proton11
./claunch-skyrail-adreno.sh
```

或自定义日志路径:
```bash
VK_TEST_LOG=$HOME/proton11/.build/adreno-skyrail.log ./claunch-skyrail-adreno.sh
```

**启动器内部关键环境变量** (手动跑时也要带全):
```
FAKE_MACHINE_ID=85536ceb47e8aa768973fe1c6a227604      # 只读 machine-id 劫持
LD_PRELOAD=$HOME/fake_machineid.so:$PROJ/.build/vulkan_gpu.so
WINEPREFIX=$PROJ/p11prefix
DISPLAY=:1
WINEESYNC=1                                            # 保持开启 (64 位安全)
PULSE_SERVER=tcp:127.0.0.1:4713
WINEDLLOVERRIDES="winepulse.drv=b;winealsa.drv="
VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json   # 指定 shim ICD
VK_LOADER_DEBUG=error,warn
DXVK_LOG_LEVEL=info
BOX64_MMAP32=1
BOX64_DYNAREC_SAFEFLAGS=2
BOX64_DYNAREC_BIGBLOCK=3
BOX64_DYNAREC_CALLRET=2
BOX64_DYNAREC_FORWARD=1024
BOX64_DYNAREC_ALIGNED_ATOMICS=1
BOX64_DYNAREC_STRONGMEM=2
BOX64_DYNAREC_WEAKBARRIER=1
BOX64_DYNAREC_FASTNAN=1
BOX64_DYNAREC_FASTROUND=1
BOX64_DYNACACHE=1
BOX64_DYNACACHE_FOLDER=$PROJ/.cache/dynacache/skyrail-adreno
BOX64_RCFILE=$PROJ/box64/system/box64.box64rc
BOX64_LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib:$PROJ/xaw64_wine/proton-11/lib/wine/x86_64-unix:$PROJ/xaw64_wine/x86_64-windows:$PROJ/proton-11/lib
LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib
```

执行: `"$PROJ/box64/build/box64" "$PROJ/xaw64_wine/proton-11/bin/wine" game.exe`

### 5.3 调试环境变量

| 变量 | 作用 |
|------|------|
| `VK_TEST_LOG=/path` | 重定向启动日志 |
| `VULKAN_WSI_DEBUG_LEVEL=3` | 打开 WSI 层 INFO 级日志 |
| `VK_GPU_LOG=1` | vulkan_gpu.so 的 namespace 链接日志 |
| `VK_ICD_BT=1` | shim 崩溃回溯 (SIGSEGV 时打印地址) |
| `VK_ICD_REBUILD=1` | shim 实验: 重建干净 VkDeviceCreateInfo (需重编译启用 EXPERIMENT_REBUILD) |

### 5.4 查看画面

通过 VNC 客户端连接 `localhost:5901`。或截图验证 (无头环境客观判断):
```bash
# 截取整个 X root 窗口
import -window root /tmp/screen.png
# 像素分析: 非纯黑即有内容
convert /tmp/screen.png -colors 8 -format "%c" histogram:info:- | sort -rn | head
convert /tmp/screen.png -resize 1x1 txt:-
```

---

## 6. 验证证据 (fix9)

日志: `~/proton11/.build/adreno-skyrail-fix9.log`
截图: `~/proton11/.build/skyrail-fix9-screen.png` (1280x720, 977,423 字节 — 纯黑屏远小于此)

| 指标 | fix8 (修复前) | fix9 (修复后) |
|------|--------------|--------------|
| `xcb_flush` 失败次数 | **数百条** | **0 条** |
| DRI3 检查 | 杀连接 (has_error=2) | 5 次 "DRI3 not available" 安全返回 |
| SHM 呈现 | 全失败 | 5 次 fallback 正常持续 present |
| 深度纹理 | Cannot create texture → 崩溃 | fmt=130 放宽 → VK_SUCCESS |
| 画面 | 黑屏有声音 | **出画面 + 声音 + 可操作** |

截图直方图 (8 色) 证明真实画面 — 白字蓝底标题页:
```
258374 px  #192B57  深蓝 (夜空/背景)
194361 px  #121624  暗蓝
134268 px  #EDEBE7  近白 (文字/UI)
104748 px  #2D4B8F  蓝
...
平均色 (80,92,122) #505C7A 蓝灰
```

日志关键行:
```
INFO surface.cpp:172: Surface 0x280002f using its own xcb connection to :1
INFO swapchain.cpp:301: x11 swapchain: DRI3 not available      (×5, 安全)
INFO swapchain.cpp:352: x11 swapchain: using SHM fallback presenter (×5)
[VK_ICD] iffp 深度格式放宽: fmt=130 tiling=0 usage=0x23 -> VK_SUCCESS  (×N)
[VK_ICD] real vkCreateDevice -> 0 (VK_SUCCESS)                        (多次)
info: Present mode: VK_PRESENT_MODE_FIFO_KHR
info: Image count: 5
```

---

## 7. 排查手册 (按症状)

### 7.1 无 namespace 链接日志
`LD_PRELOAD` 没生效, 或 shim 里 namespace 链接失败 → 检查 `VK_GPU_LOG=1`, 看有没有 `[VK_GPU] namespace 链接`。没有 → 确认 preload 路径。

### 7.2 vkCreateDevice 返回 -7 (EXTENSION_NOT_PRESENT)
shim 已剥离 win32 平台扩展 (`VK_KHR_external_semaphore_win32` 等黑名单), 若仍报, 看 shim 日志哪条扩展被拒绝, 加入黑名单或检查 wsi_keep 白名单。

### 7.3 vkCreateDevice 返回 -12 (OUT_OF_HOST_MEMORY)
DXVK 的 pNext 链带了重复 `VkPhysicalDeviceFeatures2`, 违反规范 (pEnabledFeatures 非空时 pNext 不应含 Features2)。shim 已整体剥离 pNext 链 (Adreno 540 不支持附加特性结构)。确认日志有 `[VK_ICD] pEnabledFeatures=..., 剥离 pNext`。

### 7.4 画面黑屏但日志报 xcb_flush failed
1. 先查 `has_error` 值: **2 = XCB_CONN_CLOSED_EXT_NOTSUPPORTED** (扩展查询杀连接), 不是 fd 问题
2. 检查日志是否有 "DRI3 not available" — 没有说明没走修复后的 dri3_presenter (旧 .so)
3. **重新构建并重装 WSI 层** (见 §8.2), 重启游戏

### 7.5 Cannot create texture / 深度纹理失败
检查 shim 日志 `iffp called: fmt=...` 与 `iffp 深度格式放宽`。没触发 → 格式不在白名单 (看 fmt 值) 或 usage 没带 0x20。

### 7.6 XIO: fatal IO error 9
WSI 层还在用共享连接 → 确认 `surface.cpp` 的独立连接代码在 .so 里, 日志应有 `Surface ... using its own xcb connection`。

### 7.7 32 位游戏 (9-nine) vkCreateDevice 返回 -1
见 §9。根因在 box64 + 32 位 wine 进程环境, 与 shim/参数/WSI 均无关 (同参数 64 位与原生进程都成功)。

---

## 8. 重新构建与重装

### 8.1 重新构建 ICD shim
```bash
cd ~/proton11/.build
gcc -shared -fPIC -o vulkan_adreno_icd.so vulkan_adreno_icd.c -ldl
```
(如需实验开关: 用 `-DEXPERIMENT_MIN_FEATURES` / `-DEXPERIMENT_REBUILD`)

### 8.2 重新构建并重装 WSI 层
```bash
cd ~/proton11/.build/vulkan-wsi-layer/build
make -j4
cp libVkLayer_window_system_integration.so /data/data/com.termux/files/usr/share/vulkan/implicit_layer.d/
```
> 层清单 `library_path` 是 `./libVkLayer_window_system_integration.so` (相对清单目录), 所以 `.so` 必须与 `.json` 同目录。

### 8.3 清理并重启
```bash
kill -9 $(pgrep wineserver)   # wineserver -k 在 ESYNC 不匹配时静默失败, 必须 kill -9
sleep 1
./claunch-skyrail-adreno.sh
```

---

## 9. 已知限制与后续任务

### ✅ 已完成 (2026-08-08)
- P1: 64 位 sky-rail 上屏 — **完成**, 用户确认
- X11 呈现走 SHM (CPU 拷贝) — 预期降级 (Xvnc 无 DRI3/Present), 无待修项

### ⏳ P2: 9-nine (32 位) — 当前唯一目标
- 现象: 32 位 wine 子进程 `vkCreateDevice` 返回 **-1**
- 已排除: PD 句柄 / pNext 内容 / pEnabledFeatures 组合 / 扩展名集合 / queue 参数 / layerCount/flags/alloc / 完整 VkDeviceCreateInfo 重建 (REBUILD) / g_inst↔pd 组合
- 对照: 同代码同参数, 64 位进程 VK_SUCCESS, 原生 ARM64 进程 (`direct_test.c`) VK_SUCCESS, 系统 loader→shim (`repro_game.c`) 成功
- **结论: 根因在 box64 + 32 位 wine 调原生 ARM64 shim 的 ABI/进程层面**, 与 shim 逻辑/参数无关
- 候选方向: shim 多实例/进程修复, 深挖 box64 32 位调用路径

### 🔶 P3: 备选方案
- VirGL + wined3d 走 GPU OpenGL 绕过 Vulkan (VirGL Adreno EGL 已验证, `proton11-virgl` 启动器)

### ⚠️ 其他注意
- **32 位游戏在 ESYNC 下 `virtual_setup_exception` 崩溃** → 清除残留 ESYNC server (`kill -9 $(pgrep wineserver)`)
- `wineserver -k` 静默失败 → 必须 `kill -9`
- 中文字体/Gecko 等见项目根 AGENTS.md

---

## 10. 文件速查表

| 文件 | 作用 |
|------|------|
| `~/proton11/claunch-skyrail-adreno.sh` | **64 位 Adreno 启动器 (主)** |
| `~/proton11/claunch-9nine-adreno.sh` | 32 位 9-nine Adreno 启动器 (未通) |
| `~/.codebuddy/.../memory/adreno_vulkan_wsi_verification.md` | 验证过程记忆 (根因/证据/教训) |
| `~/proton11/.build/vulkan_adreno_icd.c` | shim 源码 (深度放宽 + 扩展过滤) |
| `~/proton11/.build/vulkan_gpu.c` | namespace 绕过源码 |
| `~/proton11/.build/vulkan-wsi-layer/wsi/x11/dri3_presenter.cpp` | DRI3 查询修复 (§4.3) |
| `~/proton11/.build/vulkan-wsi-layer/wsi/x11/surface.cpp` | 独立 xcb 连接修复 (§4.1) |
| `~/proton11/.build/ext_query_test{1,2,3}` (+`.c`) | libxcb 杀连接证据链 |
| `~/proton11/.build/imgfmt_probe` (+`.c`) | 深度格式 -11 探针 |
| `~/proton11/.build/direct_test` (+`.c`) | 原生进程复刻游戏参数 (对照) |
| `~/proton11/.build/skyrail-fix9-screen.png` | 成功截图 |
| `~/proton11/.build/adreno-skyrail-fix9.log` | 成功日志 (0 flush 失败) |

---

*文档日期: 2026-08-08。与 `AGENTS.md`、记忆文件 `adreno_vulkan_wsi_verification.md` 保持一致。*
