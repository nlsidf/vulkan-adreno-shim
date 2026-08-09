# Magical Charming 字体崩溃排查与根治记录（font-error.md）

> 本文记录 Magicical Charming（以下简称 `magicha`，BGI/Buriko 32 位引擎汉化版）在
> Proton11（Termux / Wine + Box64 + DXVK-Sarek）环境下「黑屏 → 无声 → 进程内存归零冻结」
> 的完整排查、反汇编定位与最终根治过程。目标是让未来遇到同类问题时无需重新侦查。

---

## 0. 一句话结论

汉化版游戏硬编码请求字体 **`SourceHanSansSC-Bold`（思源黑体 SC Bold）**，但 prefix 内没有
该字体，游戏回退到 `Segoe UI` → Wine 的 `FontSubstitutes` 把 `Segoe UI` 替换成 `WenQuanYi
Micro Hei` → 游戏自身的字体名匹配函数用 `wcscmp` 比对「请求名」与「实际加载字体回报名」，
二者永远不相等 → **无限递归约 8787 层 → `STATUS_STACK_OVERFLOW (0xc00000fd)` → 进程崩溃，
RSS 归零**。修复方法是：为游戏请求的每个字体名，安装一份「内部 name 表等于该请求名」的字体
文件，并在注册表的 `Fonts` 注册键下直接登记（**绕开 `FontSubstitutes`**），从而让 `wcscmp`
必定命中、递归消失。

---

## 1. 环境与约束

- **运行栈**：Windows `.exe` → DXVK-Sarek v1.12.0（D3D→Vulkan）→ lavapipe / Adreno 540 →
  Wine (Proton 11) → Box64 (x86_64→ARM64) → Termux/Android。
- **游戏**：`magicha.exe`（1284608 字节，2022-05-20），BGI/Buriko 引擎，**汉化版**。
- **游戏目录**：`~/proton11/p11prefix/drive_d/magiccharming/magiccharming`
  （与 `drive_c/windows` 为同一 inode，`drive_c`/`drive_d` 的 `windows` 目录等价）。
- **硬约束**：项目路径**禁止出现中文字符**（用户明确规则）。所以字体源、脚本、日志一律放
  英文路径（`~/proton11/font-fix/`、`~/jpfont/`、`~/proton11/.build/`）。
- **关键事实**：其它游戏能正常跑，用户确认不是 Wine/Box64/DXVK 的锅。

---

## 2. 症状与初步排查

### 2.1 现象
- 游戏窗口弹出，但**纯黑屏、无声音**；
- 一段时间后**进程冻结**，用 `htop` 观察确认**常驻内存（RSS）掉到 0**；
- 反复启动表现一致，可稳定复现。

### 2.2 已排除的假设（重要，避免走回头路）
| 假设 | 结论 | 证据 |
|------|------|------|
| Wine / Box64 / DXVK 兼容性问题 | 否 | 其它游戏正常；DXVK 交换链（swap chain）初始化成功 |
| `wined3d` 回退可解 | 否 | 用户早已用 wined3d 试过，与 DXVK 表现一致 |
| 不支持 1280×720 分辨率 | 否 | 交换链正常建立，渲染管线通，问题不在呈现尺寸 |
| 缺日文字体需 winetricks | 否（但 MS Gothic/Mincho 改名仍有益） | 见第 4、5 节 |

> 排查哲学（来自用户反馈，已记入项目 memory `feedback_debug_approach.md`）：
> **先定位二进制级根因，再谈修复；拒绝「换个组件试试」式试错。**

---

## 3. 侦查手段的搭建

### 3.1 用 `cmd /c` 抓取真实启动日志
普通双击式启动吞掉 stdout/stderr，看不到游戏内部错误。改用：

```
cmd /c magicha.exe
```

把 Wine 的 `WINEDEBUG` 通道与游戏输出一起落盘。

### 3.2 调试启动脚本 `~/proton11/font-fix/run-dbg.sh`
核心环境变量：

```sh
WINEDEBUG="${DBG_CH:-+debugstr,+seh,+loaddll}"   # 关键通道：seh/loaddll/font
VK_GPU_LOG=0                                      # 关闭 GPU 日志噪音
DXVK_LOG_LEVEL=warn                               # 只留 DXVK 警告
# 日志落盘到 $VK_TEST_LOG（默认 magiccharming-dbg.log）
# 脚本开头强制 kill -9 wineserver（见第 8 节 ESYNC 说明）
```

正式运行脚本 `~/proton11/font-fix/run.sh` 的关键设置（供对照）：

```sh
ulimit -s 8192                 # 增大栈，避免早期被栈限制误伤
BOX64_MMAP32=0
WINEESYNC=1                    # 64 位可用 ESYNC；32 位游戏 ESYNC 下会崩，本作为 32 位需注意
VK_ICD_FILENAMES=.../vulkan_adreno_icd.json
LD_PRELOAD="$HOME/fake_machineid.so:$PROJ/.build/vulkan_gpu.so"
DIR="$PROJ/p11prefix/drive_d/magiccharming/magiccharming"
```

### 3.3 字体专用追踪
用 `WINEDEBUG=+font` 抓全量字体枚举/加载日志（产出 `font-trace.log` / `font-trace2.log`），
这是定位元凶的决定性证据（见第 6 节）。

---

## 4. 第一次假设：缺日文字体（MS Gothic / MS Mincho）

### 4.1 winetricks 日文字体？
用户问「是否需要 winetricks 安装日文字体」。结论：**不需要**——winetricks 拉的是版权字体且
在离线/受限网络下不可用，且根因并非单纯缺日文字体。

### 4.2 方案 A：从真 Windows 拷贝 MS Gothic/Mincho
设备里全盘搜索真实 `msgothic.ttf` / `msmincho.ttf` → **未找到**（Android 无系统日文字体）。
于是先写好 `~/proton11/font-fix/install-msfonts.sh` 占位，等用户提供文件。

### 4.3 方案 B：用自由字体改名顶替（最终采用）
- 下载 **IPA 字体**（IPA Font License v1.0，日本政府发布，自由可再分发）：
  - 官方站 `moji.or.jp` 连接被重置 → 改用清华镜像
    `mirrors.tuna.tsinghua.edu.cn/debian/pool/main/f/fonts-ipafont/`（取到 00303-23 版 deb）。
- 用 `~/proton11/font-fix/mkmsfonts.py` 从 IPA 生成 5 个改名字体：

  | 输出文件 | 来源 IPA | 改名目标 |
  |----------|----------|----------|
  | `msgothic.ttf`   | ipag   | MS Gothic |
  | `msgothicp.ttf`  | ipagp  | MS PGothic |
  | `msuigothic.ttf` | ipag   | MS UI Gothic |
  | `msmincho.ttf`   | ipam   | MS Mincho |
  | `msminchop.ttf`  | ipamp  | MS PMincho |

  脚本把 nameID 1/2/4/6 写入 **WIN_EN (0x0409 ASCII)** 与 **WIN_JA (0x0411 cp932)** 两条记录，
  并**逐字节对齐** exe 中硬编码的 cp932 字符串（见第 5 节验证），移除 nameID 16/17/21/22，
  保留 IPA 许可（nameID 0/13/14）。
- 用 `~/proton11/font-fix/install-msfonts.sh` 把字体拷进 `drive_c/windows/Fonts/`，并移除 5 条
  `FontSubstitutes` 的 MS* 替换项、清空 `HKCU\Software\Wine\Fonts*` 缓存、备份注册表。

**结果：字体装好了，但崩溃依旧。** 说明日文字体不是真正的元凶——只是有益加固。

---

## 5. 反汇编与根因定位（二进制级）

### 5.1 定位字体名匹配函数
对 `magicha.exe`（x86 PE）做反汇编（`objdump` / 等价 i386 反汇编器），定位到游戏的字体匹配
例程，关键地址（VA/偏移以实际为准）：

- **字体匹配/递归函数**：`0x432760`
  - 通过 **IAT 槽 `0x4f00a4`** 调用 `GetOutlineTextMetricsW`；
  - 把「请求字体名」与 `GetOutlineTextMetricsW` 回报的
    `otmpFaceName`（`0x432864`）/ `otmpFamilyName`（`0x4328b0`）做 **`wcscmp`**；
  - 若不相等则跳回自身 → **递归边在 `0x4329c5`**；
  - 递归深度约 **8787 层**后触发 **`STATUS_STACK_OVERFLOW (0xc00000fd)`**。
- **`IsJapaneseLocale()`**：`0x47a930`
  - 检查 `LANGID & 0x3ff == 0x11`（0x11 = 日语）→ 决定选用 cp932 全角名还是 ASCII 名。
  - 这解释了为什么游戏对日文字体同时要求 ASCII 名（`MS Gothic`）与 cp932 全角名
    （`ＭＳ ゴシック`）。

### 5.2 硬编码字体名（已实测验证）
从 exe 二进制直接读出，cp932 解码干净无乱码：

| 偏移 | 内容 | 含义 |
|------|------|------|
| `0x10e1a4` | `ＭＳ ゴシック` | MS Gothic（全角 cp932） |
| `0x10e18c` | `ＭＳ 明朝`   | MS Mincho（全角 cp932） |

`mkmsfonts.py` 生成的字体 name 表与这两串**逐字节一致**（这是方案 B 有效的前提）。

### 5.3 字体名表的双记录机制
TrueType/OpenType 的 `name` 表可为同一逻辑名登记多条不同语言记录
（Windows 平台 0x0409=ASCII、0x0411=cp932）。游戏按当前 locale 选其中一条做 `wcscmp`，
所以改名字体必须**两条都对齐**，否则日文 locale 下仍会匹配失败。

---

## 6. `+font` 追踪揭示真正元凶

重新审视 `WINEDEBUG=+font` 日志（`font-trace.log`），发现致命信号：

- 游戏对 **`Segoe UI`** 的 `EnumFonts` 调用高达 **26833 次**（≈ 2 × 13425）；
- 同期的 **`code=c00000fd`（`STATUS_STACK_OVERFLOW`）** 出现 **13425 次**；
- 而 `SourceHanSansSC-Bold` 在日志里被请求、却**找不到对应字体文件**。

结合第 5 节递归逻辑，真相浮现：

### 6.1 真正的崩溃链
```
游戏(汉化版) 请求 "SourceHanSansSC-Bold"
   └─ charset 顺序探测 128/136/134/0 (SHIFTJIS / BIG5 / GB2312 / ANSI) 全部失败
        └─ 游戏回退到 "Segoe UI"
             └─ Wine FontSubstitutes: Segoe UI → WenQuanYi Micro Hei
                  └─ 加载 wqy-microhei.ttc，GetOutlineTextMetricsW 回报名 = "WenQuanYi Micro Hei"
                       └─ 游戏 wcscmp("SourceHanSansSC-Bold"/"Segoe UI" , "WenQuanYi Micro Hei") ≠ 0
                            └─ 递归 0x4329c5 → 8787 层 → STATUS_STACK_OVERFLOW → 进程死亡(RSS=0)
```

**最初的「缺 MS Gothic」只是表象**；真正卡死的是汉化版特有的 `SourceHanSansSC-Bold` 缺失。
方案 B 之所以「装了字体仍崩」，正是因为没补 `SourceHanSansSC-Bold`。

### 6.2 为何 wcscmp 必败（核心机理）
游戏要求的是「请求名 X」，但经 `FontSubstitutes` 换成了字体 Y，而 Y 的 name 表回报的是
它自己的真名 Z（`Z ≠ X`）。`wcscmp(X, Z)` 永不等 → 递归。只要走 `FontSubstitutes` 通道，
**任何不匹配的替换都会触发递归**。根治办法是让「最终加载的字体文件，其 name 表 == 请求名 X」，
且通过 `Fonts` 注册键**直接命中**，根本不经过 `FontSubstitutes`。

---

## 7. 字体生成：补齐 SourceHanSansSC-Bold

### 7.1 源字体
- 自由字体 **Noto Sans CJK SC**（`~/.cache` 或下载的 `NotoSansCJK-*.ttc`）。
  Noto Sans CJK 与 Source Han Sans 是同一套字形（两种发行名），采用 **SIL OFL** 许可，可改名再分发。
- 下载踩坑：Tsinghua 镜像的 `noto-main.deb`（含 Bold）多次在 ~54MB 处断开；
  退而求其次用 `NotoSansCJK-Medium.ttc`（extra deb 中）生成——**字形相同，仅字重略轻**。
  真 Bold 字重留作可选优化（见第 11 节）。
- `/tmp` 在 proot 下只读 → 临时改用 `~/ftchk` 存放 fontTools wheel（后已清理）。

### 7.2 `~/proton11/font-fix/mkhanfont.py`
- 从 Noto Sans CJK SC 的某个 face（按 `startswith("Noto Sans CJK SC")` 选取）生成
  **`SourceHanSansSC-Bold.otf`**；
- 把 nameID 1/2/4/6 统一改为 `SourceHanSansSC-Bold`；
- **保留 OS/2 `ulCodePageRange1` 原值 `0x602e0107`**（= 1252/932/936/949），
  复刻真 Windows 的字符集探测，使游戏按 charset 128/136/134/0 探测时都能命中；
- 移除 nameID 16/17/21/22。
- 用法：`python3 ~/proton11/font-fix/mkhanfont.py <NotoSansCJK-*.ttc>`

### 7.3 最终字体清单（`~/jpfont/out/`，49M，已 md5 校验与 prefix 内一致）
| 文件 | 内部名 | 用途 |
|------|--------|------|
| `SourceHanSansSC-Bold.otf` | SourceHanSansSC-Bold | 汉化版主字体（根因修复） |
| `msgothic.ttf`   | MS Gothic / ＭＳ ゴシック | 日文 |
| `msgothicp.ttf`  | MS PGothic | 日文 |
| `msuigothic.ttf` | MS UI Gothic | 日文 |
| `msmincho.ttf`   | MS Mincho / ＭＳ 明朝 | 日文 |
| `msminchop.ttf`  | MS PMincho | 日文 |

---

## 8. 注册表通道陷阱（最易踩坑的一步）

Wine 字体相关注册表有**三条相互独立的通道**：

1. `HKCU/LM\...\Software\Microsoft\Windows NT\CurrentVersion\Fonts`
   —— 字体**文件登记**（`"MS Gothic (TrueType)"="msgothic.ttf"`）。这是「直接命中」通道。
2. `...\FontSubstitutes` —— 字体**替换**（独立通道，正是它引发了递归）。
3. `HKCU\Software\Wine\Fonts\External Fonts` —— Wine 的**字体缓存**，需清空后重扫。

### 8.1 第一次失败的根因
只删了 `FontSubstitutes` 的 MS* 条目，崩溃仍在（13425 次）。排查发现 `Fonts` 注册键里
`"MS Gothic (TrueType)"` 仍指向 `"wqy-microhei.ttc"`——即「登记通道」还把 MS Gothic 指向了
WenQuanYi，加载后回报名仍是 WenQuanYi，递归照旧。

### 8.2 正确做法（写入 `install-msfonts.sh` / 手动 python 改 `system.reg`）
- 在 `Fonts` 注册键写入**所有 6 个**直接命中项：
  ```
  "MS Gothic (TrueType)"="msgothic.ttf"
  "MS PGothic (TrueType)"="msgothicp.ttf"
  "MS UI Gothic (TrueType)"="msuigothic.ttf"
  "MS Mincho (TrueType)"="msmincho.ttf"
  "MS PMincho (TrueType)"="msminchop.ttf"
  "SourceHanSansSC-Bold (OpenType)"="SourceHanSansSC-Bold.otf"
  ```
- 移除 `FontSubstitutes` 中 MS* 与可能串味的条目（保留 Arial/SimSun→WenQuanYi 等无害映射）；
- 清空 `HKCU\Software\Wine\Fonts*` 缓存；
- 字体文件拷入 `drive_c/windows/Fonts/`（= `drive_d/windows/Fonts/`，同 inode）。

### 8.3 ESYNC 与 wineserver（改注册表前的必做项）
Termux proot 无 `/dev/shm`，ESYNC 下 `wineserver -k` 会**静默失败**。编辑注册表前必须：

```sh
kill -9 $(pgrep wineserver)    # 强制杀，否则改动不生效
```

### 8.4 注册表备份（回滚安全）
修复过程中的历史 `.bak` 快照（bak-msfonts / bak-fontskey / bak-han / bak-dupfix）均已删除——
它们都摄于修复途中，是**崩溃中间态**，还原任何一个都会让游戏重新崩；且位于 prefix 内，
prefix 丢失时一同消失，无实际回滚价值。

取而代之：把**当前已验证正常的**注册表在 prefix **外**存一份真正的安全网：
```
~/proton11/font-fix/system.reg.good
~/proton11/font-fix/user.reg.good
```
- 用途：prefix 被清/换机时，先把这两份拷回 `p11prefix/` 对应位置即可回到「游戏正常」状态，
  再按需跑 `install-msfonts.sh` 重做字体登记。
- 注意：改注册表前仍应 `kill -9 wineserver`；每次确认修复有效后，刷新这两份 `.good` 快照。

### 8.5 双 Fonts 键对齐（收尾发现并修复）
Wine 的字体登记其实分布在**两个键**：
- `[Software\Microsoft\Windows\CurrentVersion\Fonts]`（legacy / Win9x 键）
- `[Software\Microsoft\Windows NT\CurrentVersion\Fonts]`（现代键，Wine 实际读取）

早期只改了 NT 键（`msgothic.ttf`），legacy 键仍残留 `"MS Gothic (TrueType)"="wqy-microhei.ttc"`。
虽然当前 Wine 读 NT 键、游戏已能运行，但 legacy 键里的 wqy 指向是隐患（一旦走到日文文本、
或 prefix 被重建，可能触发回报名不匹配递归）。最终把 legacy 键的两条也改为 `msgothic.ttf`
/ `msmincho.ttf`，**两个键完全一致**，彻底消除歧义。改前务必 `kill -9 wineserver` 并备份。

---

## 9. 验证（修复有效性的硬证据）

重跑游戏并抓 `font-trace2.log`，对比前后：

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| `Segoe UI` 的 `EnumFonts` 次数 | 26833 | **0** |
| `code=c00000fd`（栈溢出）次数 | 13425 | **0** |
| `SourceHanSansSC-Bold` 被解析并插入 | 缺失 | **6 次**（字体生效） |

进程不再崩溃、窗口正常出字、黑屏消失。用户两次确认「成功了！！！」。

---

## 10. 收尾清理（磁盘占用最小化 + 冗余源删除）

- `~/jpfont`：612M → 49M（前期清理 deb/中间文件）→ **已整体删除（49M）**。理由：6 个字体已装入
  prefix 的 `drive_c/windows/Fonts/`，本地源为冗余；重装方法见第 14 节，无需保留本地副本。
- 删除 `~/proton11/.build/font-trace.log`（62M，修复前 trace）与 `~/ftchk/`（1.2M，fontTools wheel）。
- 额外修复：对齐 legacy 与 NT 两个 `Fonts` 键（见 8.5），新增备份 `system.reg.bak-dupfix-<ts>`。
- **保留**：`font-trace2.log`（5.9M，修复后证据）、`magiccharming-dbg.log`（2.1M，seh/loaddll）、
  全部注册表备份（回滚用）。
- 共释放约 **112MB**（jpfont 49M + font-trace.log 62M + ftchk 1.2M）；排查乱码与回滚能力齐全。

---

## 11. 经验沉淀与可复现清单

### 11.1 同类问题的通用定位套路
1. 黑屏/无声/RSS 归零 → 先想「是不是崩在字体/栈溢出」，而不是渲染。
2. `WINEDEBUG=+font,+seh` 看 `EnumFonts` 次数与 `code=c00000fd`。
3. 反汇编定位字体名匹配函数，看它是否用 `wcscmp(请求名, 回报名)` 且不匹配时递归。
4. **根治铁律**：为游戏请求的**每个**字体名，提供「name 表 == 请求名」的字体文件，
   并在 `Fonts` 注册键**直接登记**，绕开 `FontSubstitutes`。

### 11.2 关键脚本（均在 `~/proton11/font-fix/`）
| 脚本 | 作用 |
|------|------|
| `run.sh` | 正式启动（含 ulimit/ESYNC/ICD/LD_PRELOAD） |
| `run-dbg.sh` | 调试启动（`cmd /c` + WINEDEBUG 落盘 + kill wineserver） |
| `mkmsfonts.py` | 从 IPA 生成 5 个改名日文字体（对齐 cp932 硬编码串） |
| `mkhanfont.py` | 从 Noto CJK SC 生成 `SourceHanSansSC-Bold.otf` |
| `install-msfonts.sh` | 拷字体 + 改注册表 + 清缓存 + 备份（幂等可重跑） |
| `proton11-init.sh` | 完整环境初始化 + 增强版 `install_fonts()`（双 Fonts 键登记 6 字体，已验证） |

### 11.3 现状收口（枚举已完成）
- **字体名枚举已完成**：运行时 `+font` trace 显示游戏实际请求的字体名集合为
  `{SourceHanSansSC-Bold, MS Gothic, MS Mincho, MS PGothic, MS UI Gothic, MS PMincho}`
  （外加引擎内部回退 `Segoe UI`，但已因 `SourceHanSansSC-Bold` 命中而不再被触发）。
  全部 6 个均已在 prefix 内提供「name 表 == 请求名」的字体并直接登记，**验证崩溃归零**。
- 本地源 `~/jpfont` 已删除（见第 10 节）；需重建时按第 14 节从下载链接恢复。
- 可选优化（非必须）：用更可靠镜像下载含真 Bold 的 `NotoSansCJK-Bold.ttc`，
  重跑 `mkhanfont.py` 得到更重的字重（当前用 Medium，仅字重略轻，不影响运行）。

### 11.4 `proton11-init.sh` 的 `install_fonts()` 已端到端验证（2026-08-09）
- 逻辑：字体文件缺失时从 `FONT_SRC`（缺省 `font-fix/src`，可用环境变量覆盖）用
  `mkmsfonts.py` + `mkhanfont.py` 生成改名字体并拷入 `Fonts/`；随后把 6 个字体名
  **同时直接登记进两个注册键** `[Software\Microsoft\Windows\CurrentVersion\Fonts]`（legacy）
  与 `[Software\Microsoft\Windows NT\CurrentVersion\Fonts]`（Wine 实际读取的键），并在
  `FontSubstitutes` 移除简单 `"MS X"=` 形式替换项，清掉 `HKCU\Software\Wine\Fonts*` 缓存。
- 验证方法：用真实 `system.reg` 副本跑内嵌 Python，确认 6 条登记项**两键各一份、
  续跑幂等不重复**（每条恰好出现 2 次 = legacy + NT）。结果与已验证-good 的
  `system.reg.good` 结构一致（good 注册表本就保留 `str(7)` 形式的 `MS*` 替代表，
  证明它们不是递归元凶，无需删除）。
- 修过的一个真 bug：`KEYS` 集合推导原式 `for (_d, _f, suffix, _o) in TARGETS` 却引用
  `disp`，会在写注册表前 `NameError` 崩溃（这就是之前 legacy 键只落入部分条目的根因）。
  已改为 `for (disp, _f, suffix, _o) in TARGETS`。
- 已将该逻辑实际应用到 live prefix 的 `system.reg`/`user.reg`（应用前已备份，且与
  `system.reg.good` 结构一致），prefix 现处于完整、一致、无忧状态。
- ⚠️ 操作提醒：`proton11-init.sh` 内的 `kill_all_wine` 用 `pkill -9 -f wineserver` 等模式。
  **不要在命令行里直接粘贴含 "wineserver" 字样的整段脚本**（pkill 会匹配到当前 shell 自身
  命令行而自杀）。以脚本方式运行（`bash proton11-init.sh`）则安全，因为其进程 cmdline 是
  脚本路径而非源码文本。

---

## 12. 一句话根治总结（给未来的自己）

> 黑屏无声 + RSS 归零 + `c00000fd` = 字体名 `wcscmp` 无限递归。
> 不要靠 `FontSubstitutes` 替换（必递归），要为游戏请求的**每个**字体名装一份
> 「内部 name 表 == 该名」的字体，并在 `Fonts` 注册键直接登记；改注册表前 `kill -9 wineserver`。
> 本例补的是汉化版特有的 `SourceHanSansSC-Bold`（来自 Noto CJK）与 MS Gothic/Mincho（来自 IPA）。

---

## 13. 字体下载链接（重建用）

项目路径**禁止中文**，所有下载/解包/生成在英文路径（`~/proton11/font-fix/`、临时目录）进行。

### 13.1 IPA 字体（自由，IPA Font License v1.0，来自日本政府）
- 上游：https://moji.or.jp/ipafont/ （国内网络常连接重置，不推荐）
- 实测可用镜像：https://mirrors.tuna.tsinghua.edu.cn/debian/pool/main/f/fonts-ipafont/
  - `fonts-ipafont-gothic`（含 `ipag.otf` / `ipagp.otf`）
  - `fonts-ipafont-mincho`（含 `ipam.otf` / `ipamp.otf`）
  - 版本 `00303-23`
- 取 deb 后 `ar x *.deb` 解包，字体在 `./usr/share/fonts/...`。

### 13.2 Noto Sans CJK SC（SIL OFL，与 Source Han Sans 同源）
- Debian 包：https://mirrors.tuna.tsinghua.edu.cn/debian/pool/main/f/fonts-noto-cjk/
  - `fonts-noto-cjk`：主包，含 `NotoSansCJK-*.ttc`（Bold 在此 ttc 内）
  - `fonts-noto-cjk-extra`：含 Medium 等更细字重
  - 注意：main 包较大（数百 MB），国内镜像偶有中断；若中断可先用 `fonts-noto-cjk-extra`
    里的 Medium face 顶替（字形相同，仅字重略轻），不影响运行。
- 上游：https://github.com/notofonts/noto-cjk/releases

### 13.3 WenQuanYi Micro Hei（中文 fallback，prefix 已自带）
- 由 `proton11-fonts` 安装，已在 `drive_c/windows/Fonts/wqy-microhei.ttc`，
  用于 Arial/SimSun 等无害替换，无需额外下载。

---

## 14. 重装 / 复现步骤（prefix 被清或换机时）

> 前提：`~/proton11/font-fix/` 下的脚本（`mkmsfonts.py` / `mkhanfont.py` / `install-msfonts.sh`）仍在。
> 若脚本也丢失，按第 13 节下载源字体后手动执行对应改名逻辑（见第 7 节）。

1. **准备源字体**：按 13.1/13.2 下载并解包，得到 `ipag/ipagp/ipam/ipamp` 与 `NotoSansCJK-*.ttc`。
2. **生成改名日文字体**：
   ```sh
   python3 ~/proton11/font-fix/mkmsfonts.py      # 产出 msgothic/msgothicp/msuigothic/msmincho/msminchop .ttf
   ```
   （脚本按内部约定路径读取 IPA otf；若路径不同，先调整或把 otf 放到脚本预期位置。）
3. **生成 SourceHanSansSC-Bold.otf**：
   ```sh
   python3 ~/proton11/font-fix/mkhanfont.py <NotoSansCJK-*.ttc>
   ```
4. **安装并改注册表（幂等）**：
   ```sh
   bash ~/proton11/font-fix/install-msfonts.sh
   ```
   脚本会：① `kill -9 wineserver`；② 拷贝 6 个字体到 `drive_c/windows/Fonts/`；
   ③ 在 **legacy + NT 两个 `Fonts` 键**写入直接命中项；④ 清空 `HKCU\Software\Wine\Fonts*` 缓存；
   ⑤ 备份注册表。
5. **验证**：`WINEDEBUG=+font` 启动游戏，确认 `code=c00000fd` 计数为 0、目标字体被插入、
   不再有 `Segoe UI` 枚举暴涨。

---

## 15. INSTRUCT：同类症状排查手册（给未来的自己 / 其他游戏）

**适用症状**：Wine/Box64 下某游戏「窗口能弹出 → 黑屏/无声音 → 进程冻结，htop 看 RSS 掉到 0」。
其它游戏正常（说明不是 Wine/Box64/DXVK 的锅）。

### 15.1 三步定性
1. **是崩，不是渲染问题**？黑屏 + RSS→0 = 进程死了。分辨率/DXVK/wined3d 只会花屏或报错，
   **不会**让 RSS 归零——先排除这三者（也别试「换组件」，定位根因优先）。
2. **是不是栈溢出**？抓 `WINEDEBUG=+seh` 看 `code=c00000fd`（STATUS_STACK_OVERFLOW）。
   若是 → 大概率是**无限递归**，常见于字体名匹配。
3. **是不是字体名递归**？`WINEDEBUG=+font` 看某个字体名（如 `Segoe UI`）的 `EnumFonts` 次数
   异常暴涨（上万次）→ 命中「请求名 ≠ 回报名」的 `wcscmp` 递归。

### 15.2 定位缺哪个字体
- **动态**：`+font` trace 里被反复枚举/请求却找不到对应文件的名字，就是缺的字体
  （本例：`SourceHanSansSC-Bold`）。
- **静态**：从 exe 提取字体名串（见 15.3），枚举游戏可能请求的**全部**名字，避免漏掉某场景。

### 15.3 提取 exe 内字体名（静态枚举）
```sh
# ASCII 串
strings -n 4 magicha.exe | grep -iE 'gothic|mincho|sourcehan|sim|yahei|hei|song|ming|noto'
# UTF-16LE 串（Termux 的 strings 不支持 -e，用 python）
python3 - <<'PY'
import re
data=open("magicha.exe","rb").read()
out=[];cur=[]
for i in range(0,len(data)-1,2):
    cp=data[i]|(data[i+1]<<8)
    if 0x20<=cp<0xffff: cur.append(chr(cp))
    else:
        if cur: out.append("".join(cur)); cur=[]
if cur: out.append("".join(cur))
for t in out:
    for m in re.finditer(r'(SourceHan\w*|MS [\w ]+|Sim[HSE]\w*|Microsoft[\w ]*|WenQuan\w*|[\w ]*Gothic|[\w ]*Mincho)', t):
        print(m.group(1))
PY
```

### 15.4 根治（通用铁律）
对游戏请求的**每个**字体名 X：
1. 准备一个字体文件，其 name 表（nameID 1/2/4/6，必要时含 cp932 0x0411 与 ASCII 0x0409 双记录）
   **等于 X**；
2. 把该文件拷入 `drive_c/windows/Fonts/`；
3. 在注册表 **两个 `Fonts` 键**（`Windows\CurrentVersion\Fonts` 与 `Windows NT\CurrentVersion\Fonts`）
   写入 `"X (TrueType)"="file.ttf"`，**直接命中，不走 `FontSubstitutes`**；
4. 清空 `HKCU\Software\Wine\Fonts*` 缓存；
5. 改注册表前 `kill -9 wineserver`（ESYNC 下 `wineserver -k` 会静默失败）；改后备份。

> **为什么不能靠 `FontSubstitutes`**：替换后的目标字体回报名 Z ≠ X，`wcscmp(X,Z)` 永不等 → 递归。
> 只有「name 表 == 请求名」的字体 + 直接登记，才能让 `wcscmp` 命中、递归消失。

### 15.5 何时会遇到这个坑
BGI/Buriko、KiriKiri、NScripter 等日系 AVG 引擎在 Wine 下，若 prefix 没有游戏要求的精确字体
（尤其汉化版会要求 `SourceHanSansSC-Bold` / `SourceHanSansSC` 等思源系，或日文字体
`MS Gothic` / `MS Mincho`），就会走回退→替换→递归。签名永远是：
**黑屏无声 + RSS 归零 + `c00000fd` + 某字体名 EnumFonts 暴涨**。按 15.1–15.4 处理即可。

---

## 16. 衍生坑：中文「部分缺字 / 豆腐块」（不崩，但渲染不全）

**游戏**：巧克甜恋（Amairo Chocolate_wm，BGI 汉化版，`ac_chinese.exe`）。
**症状**：窗口正常、能玩，但中文字符**部分**变成豆腐块 `□`。例如
`今天天气真好` → `今天□真好`（`气` 这类简体专用字缺失；`天` 在 JIS 里有，反而是
`气` 这种 JIS 只有 `氣`、没有 `气` 的简体字缺）。**不是崩溃、RSS 不掉零**。

### 16.1 根因（与第 0 节不同！这里是字形覆盖，不是递归）
- prefix 的 `MS Gothic` 等 MS* 字体由 IPA 改名生成（`mkmsfonts.py`），内部覆盖只有 **JIS X 0208**
  （日文字），不含简体专用汉字（如 `气`）。
- 游戏用 `MS Gothic` 渲染中文（`+font` trace 确认 `font_SelectFont L"MS Gothic" → msgothic.ttf`）。
- Wine 对 `MS Gothic` 的默认 `FontLink` 回退链指向 `SIMSUN.TTC / MINGLIU.TTC / MSYH.TTC …`，
  **但这些文件在 prefix 里不存在** → 回退链断 → 缺字直接变豆腐。

> 注意：这是「我们给 Magical Charming 做字体修复时」的副作用。修复前 `MS Gothic` 指向
> `wqy-microhei.ttc`（中文完备），巧克甜恋正常；修复后 `MS Gothic` 改成 IPA 改名体（仅日文），
> 中文覆盖丢了 → 复现本坑。两游戏对 `MS Gothic` 的需求冲突：**Magical Charming 要 name 表==请求名
> （防递归），巧克甜恋要中文完备**。

### 16.2 诊断手段（复用 +font trace）
```sh
WINEDEBUG=+font wine ac_chinese.exe 2>&1 | grep -E 'font_SelectFont|load_system_links L"MS Gothic"'
# 期望看到:
#   font_SelectFont L"MS Gothic" -> msgothic.ttf   (游戏确实走 MS Gothic)
#   load_system_links L"MS Gothic": L"SIMSUN.TTC,SimSun" ... (回退目标是死链)
```

### 16.3 根治（双层互补，且不破坏 Magical Charming）
**铁律不变：字体内部 name 必须 == 游戏请求名**，否则 Magical Charming 会 `wcscmp` 递归崩。
所以不能把 `MS Gothic (TrueType)` 直接指向 `wqy-microhei.ttc`（那会让内部名变成
`WenQuanYi Micro Hei` → 递归）。正确做法：

1. **把 wqy（中文完备）改名成 `MS Gothic` 系列**，写回 `msgothic.ttf` 等：
   ```sh
   python3 ~/proton11/font-fix/mkmsfonts_cjk.py
   # 产出 msgothic.ttf(MS Gothic) / msgothicp.ttf(MS PGothic) / msuigothic.ttf(MS UI Gothic)
   # 内部名==请求名 (防递归) + cmap 含 气/天/今… (中文完备)
   ```
   这样 `MS Gothic` 自身就带中文 glyph，无需回退。Mincho 系列留 IPA（衬线日文，不参与中文）。
2. **兜底：把死链回退目标指到 wqy**（Wine 对 MS Gothic 的回退链是硬编码、注册表 `SystemLink`
   项会被忽略，只能让目标文件存在）：
   ```sh
   cd ~/proton11/p11prefix/drive_c/windows/Fonts
   for t in MINGLIU.TTC SIMSUN.TTC GULIM.TTC YUGOTHM.TTC MSJH.TTC MSYH.TTC MALGUN.TTF; do
     ln -sf wqy-microhei.ttc "$t"
   done
   ```
   （第 0 节那种「写 `FontLink\SystemLink` 注册表」对 MS Gothic **无效**——Wine 优先用内置默认链，
   注册表项被忽略；只对无内置默认的字体如 `SourceHanSansSC-Bold` 才生效。）

### 16.4 验证
- `python3 -c "from fontTools.ttLib import TTFont; TTFont('.../msgothic.ttf').getBestCmap()[ord('气')]"`
  不抛 KeyError 即说明 `气` 已在 `MS Gothic` 内。
- 实机进游戏看中文对话不再有 `□`。
- 回滚：若需恢复 IPA 版 `MS Gothic`，备份在 `~/proton11/font-fix/backup-ipa-gothic/`。

### 16.5 持久化
`proton11-init.sh` 的 `install_fonts()` 现已改为调用 `mkmsfonts_cjk.py`（Gothic 从 wqy 生成，
自包含、无需外部 IPA 源）生成改名字体，Mincho 仍走 `mkmsfonts.py`（需 `FONT_SRC` 有 IPA）。
这样新 prefix 也天然中文完备且不会触发 Magical Charming 递归。
（注：16.3 第 2 步的「死链 symlink 兜底」后来证实**多此一举**——`MS Gothic` 自身已是 wqy 改名体、
自带中文 cmap，回退链根本用不到；这些 `SIMSUN.TTC` 等软链已在糖调修复时移除，无回归。）

### 16.6 另一坑：糖调（Sugar☆Full Tempering）**全部**豆腐块（`□□□□`）

**游戏**：糖调！（`sugarfull-tempering`，BGI 系，`糖调！.exe`）。
**症状**：原先中文正常，**在我们修完巧克甜恋后**突然**所有**字符变 `□`（不是部分缺，是整体）。
巧克甜恋 / Magical Charming 不受影响。

**根因（与 16.1 完全不同的路径，且第一次修法其实是错的）**：
- 糖调**不用 `MS Gothic`**，而是直接用 **`Tahoma`** 渲染文本（`+font` trace 实锤：
  `font_SelectFont L"Tahoma"` 出现 22000+×，`Chosen: L"Tahoma"`）。
- Wine 自带 **bundled `tahoma.ttf`**（`Z:\proton11\proton-11\share\wine\fonts\tahoma.ttf`，
  **只有拉丁字形**）会注册成 family `Tahoma`。所以游戏请求 `Tahoma` 时，应答的就是这个拉丁版 →
  中文无字形 → 全 `□`。
- **第一次错误修法**：在双 Fonts 键加 `"Tahoma (TrueType)"="wqy-microhei.ttc"`。
  这**无效**——wqy 的内部 family name 是 `WenQuanYi Micro Hei`，所以它会归到
  family `WenQuanYi Micro Hei`，而不是 `Tahoma`；裸名 `Tahoma` 仍命中 bundled 拉丁版。
  （trace 实证：修了之后 family `Tahoma` 的 face 依然来自 `.../share/wine/fonts/tahoma.ttf`，
  `Chosen: L"Tahoma"` 仍是拉丁字形 → 还是豆腐。）
- **正确根因一句话**：要让 family `Tahoma` 真正携带中文，字体**内部名必须字面等于 `Tahoma`**
  （与 16.1 的 MS Gothic 同理：内部名 ≠ 请求名就白白归到别的 family）。

**为什么之前正常、修完巧克甜恋后才坏**：
糖调原本靠「Wine 默认字体机制 + 某次状态」勉强出字；我们重排 Fonts 键、清缓存后，bundled 拉丁版
`tahoma.ttf` 干净地独占了 family `Tahoma`，中文缺口暴露。本质是糖调的 Tahoma 路径从未被正确中文化。

**根治（把 wqy 改名成内部名=Tahoma，再登记）**：
```sh
# 生成: 内部 family=Tahoma, 但 glyph 来自 wqy(中文完备)
python3 ~/proton11/font-fix/mkmsfonts_tahoma.py
#   -> drive_d/windows/Fonts/tahoma.ttf   (Regular, 内部名 Tahoma, 含 气/天…)
#   -> drive_d/windows/Fonts/tahomabd.ttf (Bold,    内部名 Tahoma Bold)
# 双 Fonts 键登记:
"Tahoma (TrueType)"="tahoma.ttf"
"Tahoma Bold (TrueType)"="tahomabd.ttf"
```
这样 family `Tahoma` 由中文完备的 wqy 改名体应答，bundled 拉丁版被同名覆盖/降级，中文不再缺字。
`FontSubstitutes` 的 `Tahoma → WenQuanYi Micro Hei` 保留作冗余兜底。

**验证（trace 实测，已通过）**：
```sh
WINEDEBUG=+font wine '糖调！.exe' 2>&1 | grep -E 'Adding face L"Tahoma" in family L"Tahoma" from'
# 期望看到来源是  C:\windows\fonts\tahoma.ttf  (不再是 .../share/wine/fonts/tahoma.ttf 拉丁版)
# 且运行期无 'notdef' / 缺字形告警 -> 中文正常
```

### 16.7 持久化补丁（糖调 Tahoma）
`proton11-init.sh` 的 `install_fonts()` 现在：
1. 字体缺失检测列表追加 `tahoma.ttf` / `tahomabd.ttf`；
2. 生成阶段调用 `mkmsfonts_tahoma.py`（与 `mkmsfonts_cjk.py` 同理，把 wqy 改名成内部名 `Tahoma`）；
3. `TARGETS` 追加 `("Tahoma","tahoma.ttf","TrueType")` 与 `("Tahoma Bold","tahomabd.ttf","TrueType")`，
   随 6 个改名字体一起写进**双 Fonts 键**，幂等、重跑不重复。
（注意：第一次写的 `wqy-microhei.ttc` 是错的，已改为 `tahoma.ttf`/`tahomabd.ttf`。）

### 16.8 广覆盖（自动兜底更多东亚/中文字体名）

**动机**：上面每款游戏都要单独追字体名太累。目标是「大部分新 AVG / 国产游戏不用再单独调」。

**关键认知（省钱省事）**：
- Wine **只 bundled 了 `tahoma.ttf` / `tahomabd.ttf` 这两个拉丁字体**会阴影同名请求；
  其它东亚字体名（Yu Gothic / Meiryo / Microsoft YaHei / SimSun / Malgun / MingLiU / Segoe UI …）
  **没有 bundled 阴影**，所以只要 `FontSubstitutes` 里把它们映射到 `WenQuanYi Micro Hei`
  （wqy 是中文完备真实字体），请求这些名就会落到 wqy → 中文正常。**这一步注册表默认已做了大半**
  （Arial/Courier/Meiryo/Yu Gothic/Microsoft YaHei/SimSun/SimHei/Segoe UI/NSimSun… 早已 →WenQuanYi）。
- 真正需要**真实改名文件**（内部名必须 == 请求名）的只有：被阴影的 Tahoma 系，以及
  想「内部名精确匹配」保万无一失的常见名。

**做法（两层互补）**：
1. **真实改名文件**（子弹穿甲，内部名精确匹配）：`mkmsfonts_more.py` 把 wqy 改名成
   14 个常见家族名写入 `Fonts/`：`Yu Gothic`/`Yu Gothic UI`/`Meiryo`/`Meiryo UI`/
   `Malgun Gothic`/`Microsoft YaHei`/`Microsoft YaHei UI`/`SimSun`/`SimHei`/`MingLiU`/
   `PMingLiU`/`Microsoft JhengHei`/`Microsoft JhengHei UI`/`Segoe UI`。
   Wine 扫 `Fonts/` 目录自动登记，family 名即这些名 → 直接中文完备。
   （只做 Regular；粗体由 Wine 对 TTF 轮廓合成 faux-bold，wqy 是 TTF，可用。）
2. **FontSubstitutes 兜底**（便宜、注册表级）：`proton11-init.sh` 的 `install_fonts()` 在
   `FontSubstitutes` 节**追加**一大票东亚/中文字体名 → `WenQuanYi Micro Hei`
   （韩文 Gulim/Batang/Gungsuh 系；日文 Yu Mincho/Hiragino 系；简体 KaiTi/FangSong/ST*/FZ* 等；
   繁体 DFKai-SB/KaiU/MingLiU_HKSCS；字重变体 Microsoft YaHei Light；以及上面 14 个文件的 UI 变体双保险）。
   已存在则跳过，**幂等**。覆盖到名字即中文化，无需再为每个游戏单独改。

**代价**：`Fonts/` 下多了 14 个约 4.6MB 的 CJK 字体，Wine 每次启动都会加载 → 字体初始化略慢
（一次性，不影响运行帧率）。磁盘充裕（122G 可用）可接受。

**仍不自动覆盖的情况**：请求名既不在 14 个真实文件里、又没被 FontSubstitutes 列到的冷门字体名
（如极偏的厂商私有字体）→ 仍可能豆腐。遇到就照 §16.6 的套路加一条改名文件或替换项即可。

