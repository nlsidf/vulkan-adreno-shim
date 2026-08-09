# Vulkan WSI Layer (Sky1 fork)

A Vulkan layer that intercepts window system integration (WSI) calls and
provides swapchain, surface, and presentation support for GPUs whose ICD
lacks native WSI -- such as the Mali vendor driver, which exposes no DRM
render node and therefore cannot use standard Mesa WSI paths.

This is a fork of [Arm's vulkan-wsi-layer](https://gitlab.freedesktop.org/mesa/vulkan-wsi-layer)
via [ginkage's fork](https://github.com/ginkage/vulkan-wsi-layer), which
added X11 MIT-SHM presentation support. Our additions center on
multi-presenter X11 support with automatic routing between three
presentation backends.

## X11 presentation modes

The layer selects an X11 presenter automatically per application:

| Priority | Presenter | Transport | Use case |
|----------|-----------|-----------|----------|
| 1 | Wayland bypass | DMA-BUF via `zwp_linux_dmabuf_v1` | Zink / GL apps under Xwayland |
| 2 | DRI3 | XCB Present extension (COPY) | Native Vulkan apps |
| 3 | SHM | CPU copy via MIT-SHM | Fallback, always available |

**Wayland bypass** detects Xwayland, opens a direct Wayland connection to
the compositor, and presents DMA-BUFs zero-copy. A 2-frame deferred buffer
release ring prevents FBO flicker caused by implicit sync races (compositor
still reading a buffer while the app clears the next frame). Bypass state
is stored in `x11::surface` and persists across swapchain recreations,
avoiding compositor window create/destroy animations.

**DRI3** uses the XCB Present extension with `COPY` semantics. It provides
proper window decorations under Xwayland and is the default for direct
Vulkan applications.

**SHM** performs a CPU-side copy via MIT-SHM shared memory. It serves as
the universal fallback. ARM NEON-optimized copy is enabled automatically on
AArch64.

### Prerequisites

Each presenter has specific runtime requirements:

| Presenter | Requires | On Sky1 |
|-----------|----------|---------|
| Bypass | Xwayland + Wayland compositor with `zwp_linux_dmabuf_v1` | GNOME/KDE Wayland session |
| DRI3 | DRM render node (`/dev/dri/renderDN`) + X server DRI3/Present | vgem provides the render node (vendor stack) |
| SHM | XCB + MIT-SHM extension | Always available |

The Mali vendor driver (`mali_kbase`) does not expose a DRM render node,
so DRI3 relies on another driver providing one. On Sky1, the GPU switcher
selects either the vendor stack (mali_kbase + Zink for GL) or the
open-source stack (Panthor + PanVK) at boot — they do not run
simultaneously. When the vendor stack is active, the vgem (Virtual GEM)
module provides a render node at `/dev/dri/renderDN` for DRI3's GEM
handle import. On systems without vgem or any other render node provider,
DRI3 is unavailable and the fallback chain is bypass → SHM.

Bypass requires an Xwayland environment — it will not work on a native
X11 (Xorg) display server since there is no Wayland compositor to present
DMA-BUFs to. On native X11, the fallback chain is DRI3 → SHM.

### Presenter availability matrix

| Environment | Bypass | DRI3 | SHM |
|-------------|--------|------|-----|
| Xwayland (X11 app on Wayland compositor) | Yes | Yes* | Yes |
| Native X11 (Xorg, no Wayland) | No | Yes* | Yes |
| Native Wayland app | N/A (uses Wayland WSI backend) | N/A | N/A |

\* Requires a DRM render node

### Performance

Bypass is 5--15% faster than DRI3 depending on resolution, since it
presents DMA-BUFs directly to the Wayland compositor without an
intermediate X server copy. DRI3 provides proper window decorations
(title bar, border, buttons) under Xwayland, which bypass does not.

| Resolution | Bypass | DRI3 | Overhead |
|------------|--------|------|----------|
| 800x600 | 311 FPS | 293 FPS | ~6% |
| 1920x1280 | 187 FPS | 166 FPS | ~11% |

Benchmark: `glmark2 -b terrain` via Zink on Mali-G720-Immortalis.

The `gpu-compat-run` wrapper from
[sky1-gpu-support](https://github.com/Sky1-Linux/sky1-gpu-support)
selects the presenter:

```
gpu-compat-run glmark2          # bypass (default for Zink, max performance)
gpu-compat-run --dri3 glmark2   # DRI3 (window decorations, ~5-15% slower)
```

### Routing defaults and rationale

The layer auto-detects the app type and selects a presenter:

| App type | Detection | Default | Rationale |
|----------|-----------|---------|-----------|
| Zink / GL | `MESA_LOADER_DRIVER_OVERRIDE=zink` or `libzink` in `/proc/self/maps` | Bypass | These apps already pay a performance tax from GL-to-Vulkan translation through Zink; bypass recovers 5--15% of that overhead. Since the app is already running through a translation layer, the additional trade-off of losing decorations is a smaller incremental compromise. |
| Direct Vulkan | No Zink detected | DRI3 | Native Vulkan apps are typically windowed and expect normal window management (title bar, resize, close). They manage their own render loop with no translation overhead, so the 5--15% DRI3 cost is less impactful. |

### Overriding the presenter

There are three ways to override the auto-detected presenter, from most
specific to broadest:

**1. Per-app config file** (`/etc/sky1/wsi-routing.conf`)

```
# Format: process_name presenter
# Presenters: bypass, dri3, shm
glmark2    dri3
furmark    bypass
blender    dri3
```

The layer reads the process name from `/proc/self/comm` and matches it
against this file at swapchain creation. A second search path at
`/usr/share/cix-gpu/wsi-routing.conf` is checked if the first is absent.

**2. gpu-compat-run wrapper flag**

```
gpu-compat-run glmark2          # bypass (default for Zink)
gpu-compat-run --dri3 glmark2   # force DRI3 for this invocation
```

The `--dri3` flag sets `WSI_NO_WAYLAND_BYPASS=1`, which disables bypass
for the entire process. Available in the
[sky1-gpu-support](https://github.com/Sky1-Linux/sky1-gpu-support) package.

**3. Environment variable** (broadest)

| Variable | Effect |
|----------|--------|
| `WSI_NO_WAYLAND_BYPASS=1` | Disable bypass globally, fall back to DRI3/SHM |

Can be set per-session, per-user (in `.profile`), or system-wide
(in `/etc/environment`).

**When to override:**

- Use `dri3` for windowed GL apps that need decorations (title bar,
  border, buttons) -- e.g. Blender, GIMP, windowed games
- Use `bypass` for fullscreen or performance-critical apps where
  decorations are irrelevant -- e.g. games, benchmarks
- Use `shm` as a diagnostic fallback if both zero-copy paths cause
  issues (CPU copy, very slow at high resolutions)

## Key changes from upstream

- **Multi-presenter routing** -- automatic backend selection with fallback
  chain (bypass -> DRI3 -> SHM)
- **Xwayland bypass** -- zero-copy DMA-BUF presentation to the Wayland
  compositor, bypassing X11 entirely for GL/Zink workloads
- **Deferred buffer release** -- 2-frame ring prevents implicit sync races
  between compositor reads and app rendering
- **Fast swapchain teardown** -- semaphore post on teardown avoids a 250ms
  stall in the page-flip thread
- **Vulkan 1.1 promoted extension injection** -- fixes ICDs that do not
  advertise core-promoted extensions at the device level
- **Non-fatal per-device extension check** -- allows multi-GPU
  configurations where devices expose different extension sets
- **`VK_PRESENT_MODE_IMMEDIATE_KHR`** support for X11 surfaces

## Implemented extensions

Instance extensions:
- `VK_KHR_surface`
- `VK_KHR_xcb_surface` / `VK_KHR_xlib_surface`
- `VK_KHR_wayland_surface`
- `VK_KHR_get_surface_capabilities2`
- `VK_EXT_surface_maintenance1`
- `VK_EXT_headless_surface` (optional)

Device extensions:
- `VK_KHR_swapchain`
- `VK_KHR_shared_presentable_image`
- `VK_EXT_image_compression_control_swapchain`
- `VK_KHR_present_id`
- `VK_EXT_swapchain_maintenance1`

## Building

### Dependencies

- CMake >= 3.4.3
- C++17 compiler
- Vulkan loader and headers (1.1+)
- libdrm
- libwayland-client, wayland-protocols, wayland-scanner
- libxcb, xcb-shm, xcb-sync, xcb-dri3, xcb-present
- libX11, libX11-xcb, libXrandr

### Build

```
mkdir build && cd build
cmake .. \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_WSI_X11=ON \
    -DBUILD_WSI_WAYLAND=ON \
    -DSELECT_EXTERNAL_ALLOCATOR=dma_buf_heaps \
    -DWSIALLOC_MEMORY_HEAP_NAME=system \
    -DENABLE_WAYLAND_FIFO_PRESENTATION_THREAD=ON
make -j$(nproc)
```

### Install

Copy the shared library and JSON manifest into a Vulkan implicit layer
directory:

```
sudo cp libVkLayer_window_system_integration.so \
        VkLayer_window_system_integration.json \
        /usr/share/vulkan/implicit_layer.d/
```

The layer is loaded automatically by the Vulkan loader as an implicit layer.

## Upstream repositories

- **Arm (original):** <https://gitlab.freedesktop.org/mesa/vulkan-wsi-layer>
- **Ginkage (X11 SHM):** <https://github.com/ginkage/vulkan-wsi-layer>

## Related projects

This layer is part of [Sky1 Linux](https://github.com/Sky1-Linux), a Linux
distribution for systems based on the CIX Sky1 / CD8180 SoC.

- [sky1-gpu-support](https://github.com/Sky1-Linux/sky1-gpu-support) -- userspace Mali driver and GPU switcher
- [cix-gpu-kmd](https://github.com/Sky1-Linux/cix-gpu-kmd) -- Mali kernel driver module

## License

MIT. See [LICENSE](LICENSE) for details.
