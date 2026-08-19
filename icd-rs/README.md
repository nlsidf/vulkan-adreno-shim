# icd-rs — Adreno Vulkan ICD Shim (Rust 版)

`vulkan_adreno_icd.so` 的 Rust 重写实现，与 `icd/vulkan_adreno_icd.c`（C 版）功能对等，
并修复了 C 版不具备的若干命令层一致性问题。本目录产出的 `libvulkan_adreno_icd.so`
与 C 版**同名同 role**，通过 `VK_ICD_FILENAMES` 指向的 `.json` 被 Vulkan loader 加载。

环境：骁龙 835 (Adreno 540) + Termux + Box64 + Wine，跑 32/64 位 Windows 游戏。

---

## 🎯 它解决什么

1. **高位指针问题**：Adreno 540 HAL 的 `vkMapMemory` 返回 >4GB 指针，32 位 Wine WOW64
   无法表示。Shim 拦截分配/映射，注入 `VK_KHR_external_memory_fd` + `DEDICATED`
   扩展，导出 dmabuf 并自行 `mmap` 到 <4GB，零拷贝返回低位指针。详见根 `README.md`。
2. **深度格式透明替换**：Adreno 540 不支持 `VK_FORMAT_D32_SFLOAT_S8_UINT`，
   应用请求 D32S8 时，在创建/视图/RenderPass/命令层四处同步替换为硬件支持的
   `D24_UNORM_S8_UINT`（D24S8），对应用完全透明。
3. **命令层一致性**：替换后图像的清除/拷贝/blit/resolve 必须按真实格式语义转发，
   否则渲染结果错乱或黑屏（见下「黑屏根因」）。
4. **namespace 内建**：Rust 版在加载驱动时自行完成 `default↔sphal` namespace 链接，
   **不再需要单独的 `vulkan_gpu.so`**（C 版遗留文件，现已废弃）。

---

## 📁 源码结构

| 模块 | 职责 |
|------|------|
| `lib.rs` | 入口/全局状态：`vk_icdGetInstanceProcAddr` 分发、verbose 开关、`GLOBAL_DEVICE` 锚定、mem props 缓存 |
| `ffi.rs` | 手写 Vulkan/平台 FFI 类型（零第三方依赖） |
| `cffi.rs` | `dlopen`/`dlsym`/`RTLD_*` 等 libc 绑定 |
| `driver.rs` | Adreno HAL 加载：`link_namespaces()` + `dlopen vulkan.msm8998.so` + 解析 mangled `qglinternal::vkGetInstanceProcAddr` |
| `intercept.rs` | **核心**：实例/设备创建拦截、深度格式替换、命令层钩子、设备函数表解析 |
| `fmtfix.rs` | `vkGetPhysicalDeviceFormatProperties` / ImageFormatProperties 修复（深度伪装 + BC/RTV 开关） |
| `memalias.rs` | 低位 dmabuf 重映射（分配/映射/导出 fd） |
| `alloc.rs` | ICD 内部分配器选取 |
| `macros.rs` | 日志宏（`shim_log!` / `shim_dbg!`，受 `VK_ICD_VERBOSE` 门控） |

---

## ⚠️ 黑屏根因与修复（关键）

### 症状
原生 **D24S8**（如 Undertale / GameMaker Studio 1.4，D3D9）游戏在 Rust 版下黑屏（有声音），
而 C 版正常。

### 根因
命令层四个钩子（`vkCmdCopy/Blit/Resolve/ClearDepth`）原本**只**从 `IMAGE_FORMAT_MAP`
反查 `VkDevice`。对原生 D24S8 游戏，不触发 D32S8→D24S8 替换，地图为空 → 查不到 device
→ 钩子直接 `return`，**把整条拷贝/blit/resolve/clear 命令静默吞掉** → 黑屏。
C 版根本没有命令钩子，所以直通过去，一切正常。

### 修复（`intercept.rs` + `lib.rs`）
1. **`shim_create_device` 成功后锚定 `GLOBAL_DEVICE`**（全局设备兜底来源）；
2. **`cmd_device(src,dst)`** 解析顺序：`图像地图 → GLOBAL_DEVICE`，确保查不到图像时
   也能拿到真实函数指针并**向前转发**，绝不丢弃调用；
3. **`SUBSTITUTION_HAPPENED`**（`AtomicBool`）：本进程是否发生过 D32S8 替换。
   命令钩子据此短路——没有替换过的游戏（绝大多数）跳过 RwLock+HashMap 查找，
   开销≈C 版无钩子，但行为完全等价（地图本就为空）。

修复后：对**被替换的 D32S8 图像**行为不变（正确转发 + depth `clamp01`）；
对**其他所有格式**（D24S8 / color / BC 等）一律纯透传，与 C 版一致。
零回归、非激进。

---

## 🔧 构建

```bash
cd ~/vulkan-adreno-shim/icd-rs
# 编译 + clippy 零警告门禁（RUSTFLAGS=-D warnings），产物安装到 .build
bash build.sh
```

`build.sh` 等价于手动：

```bash
RUSTFLAGS="-D warnings" cargo clippy --release -- -D warnings
RUSTFLAGS="-D warnings" cargo build --release
cp target/release/libvulkan_adreno_icd.so ~/proton11/.build/vulkan_adreno_icd.so
```

编译产物：`target/release/libvulkan_adreno_icd.so`（cdylib）。

构建配置（`Cargo.toml`）：`panic = "abort"`、`opt-level = 3`、`lto = true`、
`codegen-units = 1`、`strip = true`。**零第三方依赖**，所有 libc/Vulkan 类型手写 FFI。

---

## 🚀 部署与运行

1. `vulkan_adreno_icd.json` 指向 `vulkan_adreno_icd.so`（同目录，无需改名）；
2. 通过 `VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json` 加载；
3. **不再需要 `LD_PRELOAD=...vulkan_gpu.so`** —— namespace 链接已由 `driver.rs`
   在 `init_driver()`（先于 `dlopen` 驱动）内完成；
4. 建议保留 `LD_PRELOAD=$HOME/fake_machineid.so`（机器 ID 伪装，与 shim 无关）。

示例启动脚本片段：

```bash
export LD_PRELOAD="$HOME/fake_machineid.so"          # 不再含 vulkan_gpu.so
export VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json
export VK_ICD_MAP_LOW=1                              # 32 位游戏低位映射
```

---

## 🐞 调试

| 环境变量 | 作用 |
|----------|------|
| `VK_ICD_VERBOSE=2` | 打开热路径诊断（`vkCreateImage` 格式、`[VK_ICD]` 初始化、命令层转发日志） |
| `VK_ICD_DIAG=1` | 设备创建诊断（扩展剥离、feature 表） |
| `VK_TEST_RAW=1` | 关闭所有格式/深度修复，纯透传真实 HAL（对照实验用；此时原生 D24S8 游戏会黑屏，证明修复必要） |
| `VK_ICD_MAP_LOW=1` | 启用低位 dmabuf 映射（32 位游戏必需） |
| `VK_GPU_LOG=1` | 驱动加载日志 |

注意：`VK_TEST_RAW=1` 下 `IMAGE_FORMAT_MAP` 不写入；双 shim（`gpu.so` + icd）场景已废弃，
单 icd 即可。

---

## 📝 与 C 版 (`icd/`) 的差异小结

- **一致**：格式修复、深度替换、低位 dmabuf 映射、扩展/feature 处理；
- **Rust 独有**：命令层一致性钩子（修复原生 D24S8 黑屏）、`GLOBAL_DEVICE` 兜底、
  `SUBSTITUTION_HAPPENED` 热路径短路、namespace 链接内建（无需 `vulkan_gpu.so`）；
- **C 版已不再是修复源**：本 Rust 版为当前生效实现，`vulkan_gpu.so` 可删除。
