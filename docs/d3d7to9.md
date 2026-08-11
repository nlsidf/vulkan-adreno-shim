# 补充：让 D3D7（及 DirectDraw）游戏在 Adreno 540 上跑起来

> 本篇是 [`../README.md`](../README.md) 的**补充**，与 ICD Shim 本身无关——Shim 一行未改。
> 它解决的是“Shim 已能让 D3D9 游戏（经 DXVK）硬件加速，但**纯 D3D7 游戏**在 Adreno 上起不来”的问题。
> 方案：在 prefix 层用 **D3D7to9（elishacloud/DXWrapper）** 把 D3D7 翻译成 D3D9，复用 Shim 已经打通的 DXVK → xlib_surface 路径。

---

## 1. 背景：Shim 管 D3D9，不管 D3D7

Shim（`icd/vulkan_adreno_icd.c`）解决的是 Vulkan 层的内存映射与 WSI 问题，使 DXVK 的 **D3D9** 能走 `vkCreateXlibSurfaceKHR` 正常呈现（9-nine 等已验证 60 FPS）。

但有一类老游戏（主要是 KiriKiri 引擎的 Galgame）用的是 **DirectDraw 7 / Direct3D 7**，它们不走 DXVK 的 D3D9，而是：

- 要么用 Wine 内置 ddraw（软件/GDI，无加速）；
- 要么用 **DXVK-Sarek 的 D7VK**（`ddraw.dll` → Vulkan）。**D7VK 走的是 `vkCreateWin32SurfaceKHR`，而本 Shim 不实现 win32_surface（源码里 `VK_KHR_win32_surface` 在黑名单中被主动剥离）。** 所以 D7VK 在 Adreno 上拿不到 surface → 无窗口 → 卡死。

> 误区：不是“D3D7 不行”，是“D7VK 这条 WSI 路径打了 Shim 的空白区”。D3D9 能跑，正是因为绕开了 win32_surface。

---

## 2. 两个真实故障

### 2.1 hokejyo：D7VK 自递归 → 栈溢出（表象是栈溢出，本质是前置条件缺失的循环）
- 游戏目录自带 Sarek D7VK `ddraw.dll`（2.7MB）。
- D7VK 初始化时 `LoadLibrary("ddraw_.dll")`（Sarek 约定：`ddraw_.dll` 才是真正的 ddraw 转发目标）。
- 整个 prefix **没有** `ddraw_.dll` → D7VK 回退加载 `ddraw.dll`（自己）→ **自我递归** → WOW64 32↔64 thunk 层 32 位栈耗尽 → `EXCEPTION_STACK_OVERFLOW (0xc00000fd)`。
- 崩溃前 D7VK 从未创建 Vulkan device，所以与 Vulkan 无关，是加载链断裂。

### 2.2 naga（Nagaruboshi / starfalling）：D3D7 游戏加载即死、无窗口
- 诊断日志关键行：
  ```
  loader_gpa_instance_terminator() unrecognized name vkCreateWin32SurfaceKHR
  loader_gpa_instance_terminator() unrecognized name vkGetPhysicalDeviceWin32PresentationSupportKHR
  ```
- 进程不退出但 CPU 98%、无窗口：D7VK 要 win32_surface，Shim 不提供 → NULL surface。

---

## 3. 方案选择

| 方案 | 做法 | 取舍 |
|------|------|------|
| A（改 Shim） | 在 `vulkan_adreno_icd.c` 实现 `vkCreateWin32SurfaceKHR` / `vkGetPhysicalDeviceWin32PresentationSupportKHR`，HWND→X11 Window→真驱动 `vkCreateXlibSurfaceKHR` | 修 D7VK 本身，最“正规”；但要动脆弱的 Shim、加 `-lX11` 重编、保证与 WSI layer 一致，风险高 |
| **B（D3D7to9 全局替换）** ✅ | 用 D3D7to9 `ddraw.dll` 把 D3D7 翻成 D3D9，复用 DXVK 已验证的 xlib_surface 路径 | 不动 Shim；避开 win32_surface 缺口与 D7VK 递归；改动集中在 prefix 的 ddraw，可回退 |

**选 B，且做成 prefix 级（不是单游戏 hack）**，让所有 32-bit D3D7 游戏受益。

D3D7to9 来源：elishacloud/DXWrapper 发行包 `dx7.games.zip`（v1.7.8400.25），含：
- `ddraw.dll`（485 KB，32-bit，D3D7to9 本体）
- `dxwrapper.dll`（8 MB，32-bit，DXWrapper 引擎——`ddraw.dll` 会 `LoadLibrary("dxwrapper.dll")`，**缺它则无法翻译**）
- `dxwrapper.ini`（`Dd7to9 = 1`）

本仓库 `../d3d7to9/` 目录已附带这三件套（与线上验证一致的版本）。

---

## 4. 部署（prefix 级）

设 `PREFIX` 为 Wine prefix 根，`WINEBU` 为 Sarek 安装时备份的 Wine 内置 ddraw 目录（如 `dxvk-sarek-1.12.0/wine-backup`）。

### 4.1 备份原 Sarek D7VK（可回退）
```bash
mkdir -p ~/ddraw-backup
cp -f $PREFIX/drive_c/windows/syswow64/ddraw.dll ~/ddraw-backup/ddraw.D7VK.syswow64.bak
cp -f $PREFIX/drive_c/windows/system32/ddraw.dll  ~/ddraw-backup/ddraw.D7VK.system32.bak
```

### 4.2 装 D3D7to9 为全局 ddraw（32-bit）
```bash
cp -f d3d7to9/ddraw.dll $PREFIX/drive_c/windows/syswow64/ddraw.dll
```
> 若 prefix 注册表已有 `"ddraw"="native"`（Sarek 安装通常已设），Wine 会自动加载它，无需改启动脚本 override。

### 4.3 装 DXWrapper 引擎（同目录，8MB）
```bash
cp -f d3d7to9/dxwrapper.dll $PREFIX/drive_c/windows/syswow64/dxwrapper.dll
```
> 否则日志报 `Failed to load module dxwrapper.dll (c0000135)`，D3D7to9 无法翻译。

### 4.4 准备“真正的” ddraw（D3D7to9 把 2D 转发给它，避免自递归）
```bash
cp -f $WINEBU/syswow64/ddraw.dll $PREFIX/drive_c/windows/syswow64/ddraw_wine.dll
```
- `ddraw_wine.dll` = Wine 内置 32-bit ddraw（**非** D7VK）。
- **关键陷阱**：`RealDllPath` 绝不能指向 D3D7to9 自己的 `ddraw.dll`（否则又自递归）。必须用一个**不同文件名**的 Wine 内置 ddraw。

### 4.5 64-bit system32 ddraw 也换成 Wine 内置（撤掉 64-bit D7VK）
```bash
cp -f $WINEBU/system32/ddraw.dll $PREFIX/drive_c/windows/system32/ddraw.dll
```
> 没有 64-bit D3D7 游戏，但留着 64-bit D7VK 会埋递归隐患；换成 Wine 内置最干净。

### 4.6 写 `syswow64/dxwrapper.ini`
DXWrapper 读**与 ddraw.dll 同目录**的 `dxwrapper.ini`（本仓库 `d3d7to9/dxwrapper.ini` 即此内容）：
```ini
[General]
RealDllPath                = C:\windows\syswow64\ddraw_wine.dll
WrapperMode                = AUTO
DisableLogging             = 0

[Compatibility]
Dd7to9                     = 1
D3d8to9                    = 0
D3d9to9Ex                  = 0
D3d9on12                   = 0
DDrawCompat                = 0
Dinputto8                  = 0
DisableGameUX              = 0
EnableDdrawWrapper         = 0
EnableD3d9Wrapper          = 0
EnableDinput8Wrapper       = 0
EnableDsoundWrapper        = 0
DisableGDIGammaRamp        = 0

[ddraw]
DdrawUseDirect3D9Caps      = 0
```

### 4.7 自带 D7VK 的游戏目录要改名
若某游戏目录自带 `ddraw.dll`（如 hokejyo 的 Sarek D7VK），它会**遮蔽**全局 D3D7to9 并再次递归。改名即可回落到全局：
```bash
mv $PREFIX/drive_c/hokejyo/ddraw.dll $PREFIX/drive_c/hokejyo/ddraw.D7VK.bak
```

最终 `syswow64` 相关文件：
```
ddraw.dll        # D3D7to9 本体（替换 Sarek D7VK）
ddraw_wine.dll   # Wine 内置 ddraw，2D 转发目标（RealDllPath 指向它）
dxwrapper.dll    # DXWrapper 引擎（被 ddraw.dll 加载）
dxwrapper.ini    # 上述配置
```

---

## 5. 踩坑记录

1. **栈溢出只是表象**：hokejyo 的栈溢出是缺 `ddraw_.dll` 导致的无限递归，不是真栈 bug。
2. **D3D7 没原罪**：D3D7 能跑，是 D7VK 的 win32_surface 打了 Shim 空白区。D3D9 能跑因走 xlib_surface。
3. **prefix 级而非单游戏**：最初把文件塞进游戏目录是单游戏 hack，已改为全局替换 `syswow64/ddraw.dll`。
4. **RealDllPath 递归陷阱**：`AUTO` 会解析到 D3D7to9 自己 → 递归。必须以不同文件名（`ddraw_wine.dll`）提供 Wine 内置 ddraw 并显式指向。
5. **缺 dxwrapper.dll 引擎**：只放 `ddraw.dll` 时无法翻译，补 8MB 引擎后解决。
6. **残留进程干扰**：测试期“无窗口/假死”多次源于之前崩溃留下的 wineserver / 游戏进程没清。启动前务必 `kill -9 $(pgrep wineserver)` 并确认进程清场。
7. **Steam 模拟器警告非致命**：诊断时弹“Steam 欺骗 dll 未加载”，查证原始启动脚本也无 steam 模拟器 override，属既有非致命提示，不影响进游戏。

---

## 6. D3D 版本覆盖说明

- **D3D7（DirectDraw 7 / IDirect3D7）**：✅ 经 D3D7to9 → D3D9 → DXVK → xlib_surface，已验证（naga 实跑进游戏）。
- **D3D ≤ 6（DX1–6 立即模式）**：不经 D3D7to9，回落到 `ddraw_wine.dll`（Wine 内置 ddraw）→ wined3d/软件渲染（OpenGL/Mesa llvmpipe），**无 DXVK 加速**，需按游戏个案验证。
- **2D DirectDraw（1–7 全版本）**：由 ddraw 包装层转发给真实 ddraw，正常。
- **64-bit**：D3D7to9 仅 32-bit（现实中 D3D7 游戏均为 32-bit，无影响）。
- **Direct3D Retained Mode（`d3drm.dll`，DX1–6 个别游戏）**：不经 ddraw.dll，D3D7to9 管不到，仍靠 Wine 的 d3drm，大概率不行。

> 想让 D3D7 及以下全部在 Adreno 上 GPU 加速，仍需走方案 A（Shim 实现 win32_surface + 真正修 D7VK）。本篇方案 B 只解决 D3D7 经 DXVK 这一条路。

---

## 7. 验证 / 回退

- **验证**：naga 在 Adreno 540 + 本 Shim 下成功进入游戏（D3D7→D3D9→DXVK→Vulkan→Adreno）。
- **回退**：把 `~/ddraw-backup/ddraw.D7VK.*.bak` 拷回 `syswow64/` 与 `system32/`，删掉 `ddraw_wine.dll` / `dxwrapper.dll` / `dxwrapper.ini` 即可恢复 Sarek D7VK 状态。

---

## 8. 许可证 / 来源

- D3D7to9 与 DXWrapper 来自 **elishacloud/DXWrapper**（MIT License）。本仓库 `d3d7to9/` 附带的是 v1.7.8400.25 的 `dx7.games.zip` 内容。
- 重新获取：
  ```bash
  curl -sL -o dx7.games.zip \
    https://github.com/elishacloud/dxwrapper/releases/download/v1.7.8400.25/dx7.games.zip
  python3 -c "import zipfile; zipfile.ZipFile('dx7.games.zip').extractall('d3d7to9')"
  ```
- `ddraw_wine.dll` **不随附**，需从你的 Wine prefix / Sarek wine-backup 的内置 ddraw 取得（非 redistributable 的 Wine 组件）。
