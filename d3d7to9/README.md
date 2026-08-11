# d3d7to9 —— D3D7→D3D9 翻译层（Adreno 540 补充修复）

让纯 D3D7 / DirectDraw 7 游戏在 Adreno 540 + Wine + 本仓库 ICD Shim 环境下跑起来。
原理：把 D3D7 翻成 D3D9，复用 DXVK 已验证的 `vkCreateXlibSurfaceKHR` 路径（Shim 本身无需改动）。

来源：elishacloud/DXWrapper v1.7.8400.25 的 `dx7.games.zip`（MIT License）。

## 本目录内容

| 文件 | 作用 |
|------|------|
| `ddraw.dll` | D3D7to9 本体（32-bit） |
| `dxwrapper.dll` | DXWrapper 引擎（32-bit，被 `ddraw.dll` 加载，**不可缺**） |
| `dxwrapper.ini` | 配置：`Dd7to9=1`，`RealDllPath` 指向 `ddraw_wine.dll` |

> `ddraw_wine.dll`（Wine 内置 ddraw，D3D7to9 转发 2D 调用用）**不在此目录**——它是 Wine 组件，
> 从你的 prefix 或 Sarek `wine-backup/syswow64/ddraw.dll` 复制为 `syswow64/ddraw_wine.dll`。

## 快速部署（prefix 级，非单游戏）

详见 [`../docs/d3d7to9.md`](../docs/d3d7to9.md)。要点：

```bash
PREFIX=你的wine_prefix
SYS=$PREFIX/drive_c/windows/syswow64

cp ddraw.dll     $SYS/ddraw.dll          # 替换 Sarek D7VK（全局生效）
cp dxwrapper.dll $SYS/dxwrapper.dll      # 引擎
cp dxwrapper.ini $SYS/dxwrapper.ini
# 从 Wine 内置 ddraw 取一份，改名避免自递归：
cp $WINEBU/syswow64/ddraw.dll $SYS/ddraw_wine.dll

# 64-bit 也换成 Wine 内置，撤掉 64-bit D7VK：
cp $WINEBU/system32/ddraw.dll $PREFIX/drive_c/windows/system32/ddraw.dll

# 若某游戏目录自带 ddraw.dll（Sarek D7VK），改名让它回落到全局：
mv $PREFIX/drive_c/hokejyo/ddraw.dll $PREFIX/drive_c/hokejyo/ddraw.D7VK.bak
```

启动前清残留进程：`kill -9 $(pgrep wineserver)`。
