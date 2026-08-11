Adreno Vulkan Shim for 32/64-bit Wine Games on Termux

https://img.shields.io/badge/License-MIT-yellow.svg

在骁龙 835 (Adreno 540) + Termux + Box64 + Wine 环境下，让 32/64 位 Windows 游戏通过原生 Vulkan 硬件加速运行的 ICD Shim + WSI 层解决方案。

---

🎯 项目背景与解决的问题

在 Android 的 Termux 环境中通过 Box64 和 Wine 运行 32 位 Windows 游戏时，一个经典且棘手的问题是：

· Adreno 540 Vulkan HAL 在 vkMapMemory 中返回的 CPU 侧指针位于 高 4GB 地址空间（例如 0x7fdb95000，约 34GB）。
· Wine 的 WOW64 层 在转换 32 位指针时，无法将超过 4GB 的指针塞入 32 位寄存器，因此主动 unmap 并合成 VK_ERROR_OUT_OF_HOST_MEMORY，导致游戏白屏但有声音。

❌ 为何其他方案失败？

尝试方案 结果
BOX64_MMAP32=1 强制所有匿名映射到低 4GB，但会破坏 Adreno HAL 内部控制结构，导致 vkCreateDevice 返回 -1
mremap 重映射别名 KGSL VMA 带 VM_PFNMAP\|VM_DONTEXPAND，内核拒绝（EFAULT）
同一 fd 二次 mmap KGSL 只允许一个用户态映射（EBUSY）
VK_EXT_external_memory_host Adreno 540 驱动不支持该扩展

✅ 我们的解法：零拷贝 dmabuf 重映射

本方案通过 Vulkan ICD Shim 在驱动层介入：

1. 自动注入 VK_KHR_external_memory_fd 等扩展，使显存可导出为 dmabuf。
2. 拦截 vkAllocateMemory，为 HOST_VISIBLE 内存创建 dummy buffer 并标记为 DEDICATED_ONLY。
3. 拦截 vkMapMemory，调用真实 HAL 建立 GPU VA，同时导出 dmabuf 并用标准 mmap 映射到 低 4GB 地址空间，将低位指针返回给 32 位 guest（Wine WOW64 可正常处理）。
4. 零拷贝：dmabuf 与原始显存共享同一物理页，无数据复制。
5. 完全绕过 Box64 的 mmap 钩子，不依赖 BOX64_MMAP32 环境变量。

---

✨ 效果展示

· Galgame（如 9-nine）：在骁龙 835 上稳定 60 FPS。
· 性能损耗：仅 vkMapMemory 时增加一次 mmap 系统调用（微秒级），运行时零额外开销。
· 兼容性：已测试 9-nine、Mashiroiro Symphony SANA、Skyrail 等多款 32/64 位游戏。

---

📁 项目结构

```
vulkan-adreno-shim/
├── README.md                     # 本文件
├── LICENSE                       # MIT 许可证
├── .gitignore
├── icd/                          # ICD Shim 核心源码（完全原创）
│   ├── vulkan_adreno_icd.c       # 主 shim 实现
│   ├── vulkan_adreno_icd.json    # ICD 清单文件（Vulkan Loader 加载用）
│   └── vulkan_gpu.c              # 可选：Android linker namespace 绕过
├── wsi/                          # Vulkan WSI Layer（基于 Sky1-Linux 修改）
│   └── vulkan-wsi-layer/         # 完整源码（已移除 .git）
│       ├── wsi/x11/surface.cpp   # 修改：独立 XCB 连接
│       └── wsi/x11/dri3_presenter.cpp # 修改：安全 DRI3 探测
├── scripts/                      # 启动脚本示例
│   ├── claunch-9nine-adreno.sh   # 32 位 Galgame 启动
│   └── claunch-skyrail-adreno.sh # 64 位 Unity 游戏启动
├── tests/                        # 独立测试程序
│   ├── t_extfd2.c                # dmabuf 基础导出测试
│   ├── t_extfd4.c                # 内存类型矩阵测试
│   ├── t_shimlow.c               # 端到端 shim 模拟测试
│   └── ...                       # 其他探测/调试工具
├── docs/
│   └── adreno32.md               # 完整技术文档（含根因分析、失败路径、实现细节）
└── archive/                      # 非核心测试/日志/备份（压缩存档）
    └── extra_files.7z
```

---

🚀 快速开始

1. 环境准备

· Termux（从 F-Droid 或 GitHub 安装，不要用 Google Play 版本）
· 已安装 box64、wine（支持 WOW64 的版本，如 Proton 11）、DXVK-Sarek 或主线 DXVK 1.10.x。
· X 服务器（如 TigerVNC）运行在 :1 显示编号。
· 已编译并安装好 Sky1 WSI Layer（vulkan-wsi-layer）为隐式层，位于 /data/data/com.termux/files/usr/share/vulkan/implicit_layer.d/。

2. 编译 ICD Shim

```bash
cd ~/vulkan-adreno-shim/icd
gcc -shared -fPIC -O2 -Wall -o vulkan_adreno_icd.so vulkan_adreno_icd.c -ldl
```

3. 配置 ICD 路径

在启动脚本中设置环境变量：

```bash
export VK_ICD_FILENAMES=/path/to/vulkan_adreno_icd.json
```

4. 启动游戏（以 9-nine 为例）

```bash
cd ~/vulkan-adreno-shim/scripts
./claunch-9nine-adreno.sh
```

该脚本会：

· 设置 BOX64_MMAP32=0（必须！）
· 指向本 shim 作为 ICD
· 启动音频、设置显示、清理残留进程
· 运行游戏并保存日志

---

🧪 验证是否生效

查看日志文件（默认 ~/.build/adreno-9nine.log），应出现类似：

```
[VK_ICD] 低位 dmabuf 映射已启用
[VK_ICD] alloc 已接管: type=2 请求=16384KB 实分=16389KB
[VK_ICD] vkMapMemory -> 低位 dmabuf 0x34000000
info:  Presenter: Actual swap chain properties: Format: ... Buffer size: 960x540
```

若出现 Mapping memory failed 错误，请确认：

· BOX64_MMAP32 是否为 0
· ICD 路径是否正确
· WSI 层是否已正确安装

---

📖 进阶文档

· docs/adreno32.md – 包含完整的根因分析、所有失败路径记录、shim 实现细节、调试技巧。
· tests/ – 独立的测试程序可用于验证 dmabuf 导出、内存类型兼容性、shim 端到端行为。

---
先告诉你结果

你可以在 Termux 里跑 Windows 游戏，用上硬件加速，画面流畅得像原生系统一样。

这件事听起来像某种技术预言，但它现在已经是现实了——一份完全开源的代码，就能做到这件事。

而这份代码只干了一件事：把两套原本没打算互相配合的软件，硬生生捏在了一起。

它解决了什么问题？

Android 上的硬件驱动，本来就是为手机 App 设计的，没考虑过 Termux 这种“非标准”环境。

· 驱动文件放在了 Android 禁止普通进程访问的地方
· 驱动接口只有手机系统才认得出来
· 驱动返回的内存地址，32 位程序根本读不懂

所以如果你想在 Termux 里跑个 Windows 游戏，那些官方渠道全都会告诉你：这条路不通。

但“不通”不代表做不到，只是没人愿意做而已。

我选择了一条没人走的路

我写了一个 shim——一个夹在系统加载器和硬件驱动之间的小模块。

它做的事情很直接：

· 打通 Android 对硬件库的隔离
· 让系统加载器“以为”这是一块标准的 GPU 驱动
· 修正驱动返回的地址，让 32 位程序也能读写

不修改系统，不刷模块，不 root。

只是给原本不兼容的两端，搭了一座桥。

这意味什么？

这意味着“兼容性”不一定要等官方施舍。

当一个场景太小众、没人愿意投入资源去适配的时候，你不需要从头造一个驱动，只需要理解两端各自在说什么，然后把它们翻译成对方听得懂的话。

这就是 shim 做的事。它不重写底层，不给系统动手术，它只是在夹缝里精准地补上那几个缺失的接口。

这个项目可以被用在哪里？

如果你希望在 Termux 上跑 Windows 程序、想在非标准环境里调用硬件加速、或者只是想知道“桥接”这件事到底能做到什么程度，那你可能会需要它。

它不是开箱即用的傻瓜式工具，但它是一套已经跑通的、完整的技术方案。

走不通的路，不一定真的是死路。

---
🤝 贡献

我们欢迎任何形式的贡献！如果你发现 Bug 或有改进建议，请提交 Issue 或 Pull Request。

---

⚖️ 许可证

· ICD Shim (icd/ 及 vulkan_gpu.c)：采用 MIT License。
· WSI Layer (wsi/)：基于 Sky1-Linux/vulkan-wsi-layer，同样采用 MIT 许可证，保留原始版权声明。
· 文档和测试程序：同样采用 MIT 许可证。

---

🙏 致谢

· Sky1-Linux/vulkan-wsi-layer – 提供了 WSI 层的稳定实现基础。
· Box64 – 强大的 x86_64 模拟器。
· Wine – 让 Windows 应用在 Linux 上运行。
· DXVK 及其分支 DXVK-Sarek – 出色的 Direct3D→Vulkan 翻译层。

---

📞 联系与支持

如果你在使用过程中遇到任何问题，欢迎在 GitHub Issues 中提出。也欢迎通过邮件或社区讨论分享你的经验。


