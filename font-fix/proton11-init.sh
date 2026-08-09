#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# Proton 11 环境初始化脚本
# 首次使用前运行，完成 wineprefix 创建、DXVK 安装、Mono 安装、Gecko 安装
# 用法: proton11-init
# ============================================================
set -e
PROJ="$HOME/proton11"
log() { echo "[Proton11-Init] $1"; }
warn() { echo "[Proton11-Init] 警告: $1" >&2; }
die() { echo "[Proton11-Init] 错误: $1" >&2; exit 1; }
WINE_BIN="$PROJ/xaw64_wine/proton-11/bin/wine"
BOX64="$PROJ/box64/build/box64"
WINEPREFIX_DIR="$PROJ/p11prefix"
DXVK_DIR="$PROJ/dxvk-2.5.3/x64"
MONO_MSI="$PROJ/wine-mono-11.1.0-x86.msi"
GECKO_MSI_X86="$PROJ/wine-gecko-2.47.4-x86.msi"
GECKO_MSI_X64="$PROJ/wine-gecko-2.47.4-x86_64.msi"
# --- 字体修复相关路径 ---
FONT_FIX="$PROJ/font-fix"
# 字体源目录: 需含 IPA 提取目录 (x-gothic/, x-mincho/) 与 Noto CJK ttc
# 可用环境变量 FONT_SRC 覆盖; 缺省指向 $FONT_FIX/src
FONT_SRC="${FONT_SRC:-$FONT_FIX/src}"
check_prereqs() {
    [ -x "$BOX64" ] || die "Box64 未找到: $BOX64"
    [ -x "$WINE_BIN" ] || die "Wine 未找到: $WINE_BIN"
    if [ ! -L "$PROJ/xaw64_wine/proton-11" ]; then
        die "xaw64_wine/proton-11 符号链接缺失"
    fi
    if [ ! -L "$PROJ/xaw64_wine/10.7-stable" ]; then
        die "xaw64_wine/10.7-stable 符号链接缺失"
    fi
    if [ ! -f "$PROJ/xaw64_wine/.wine-version" ]; then
        die "xaw64_wine/.wine-version 文件缺失"
    fi
}
kill_all_wine() {
    for name in wineserver wineboot rundll32 services winedevice plugplay svchost rpcss explorer tabtip game; do
        pkill -9 -f "$name" 2>/dev/null || true
    done
    sleep 1
}
backup_reg() {
    local f="$1"
    [ -f "$f" ] && cp -a "$f" "$f.bak-fontfix-$(date +%Y%m%d%H%M%S)"
}
setup_env() {
    unset LD_PRELOAD
    export DISPLAY=:1
    export WINEPREFIX=$WINEPREFIX_DIR
    export WINEESYNC=1
    export WINEDEBUG=-all
    export VK_ICD_FILENAMES=/data/data/com.termux/files/usr/share/vulkan/icd.d/lvp_icd.aarch64.json
    export BOX64_MMAP32=1
    export BOX64_DYNAREC_SAFEFLAGS=2
    export BOX64_DYNAREC_BIGBLOCK=3
    export BOX64_DYNAREC_CALLRET=2
    export BOX64_DYNAREC_FORWARD=1024
    export BOX64_DYNAREC_ALIGNED_ATOMICS=1
    export BOX64_DYNAREC_STRONGMEM=2
    export BOX64_DYNAREC_WEAKBARRIER=1
    export BOX64_DYNAREC_FASTNAN=1
    export BOX64_DYNAREC_FASTROUND=1
    export BOX64_RCFILE=$PROJ/xaw64_box64/etc/box64.box64rc
    export BOX64_LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib:$PROJ/xaw64_box64/lib:$PROJ/xaw64_wine/proton-11/lib/wine/x86_64-unix:$PROJ/xaw64_wine/proton-11/lib/wine/i386-windows:$PROJ/xaw64_wine/x86_64-windows:$PROJ/proton-11/lib
    export LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib:${LD_LIBRARY_PATH:-}
    export PULSE_SERVER=tcp:127.0.0.1:4713
    export WINEDLLOVERRIDES="winepulse.drv=b;winealsa.drv="
}
start_wineserver() {
    local esync=$1
    export WINEESYNC=$esync
    $BOX64 $WINE_BIN wineserver -p 2>/dev/null &
    for i in $(seq 1 10); do
        if $BOX64 $WINE_BIN cmd /c "ver" 2>/dev/null | grep -qi windows; then
            return 0
        fi
        sleep 1
    done
    return 1
}
install_msi() {
    local src="$1"
    local msi_file=$(basename "$src")
    cp "$src" "$WINEPREFIX_DIR/drive_d/"
    $BOX64 $WINE_BIN msiexec /i "C:\\$msi_file" /quiet 2>/dev/null &
    local MPID=$!
    for i in $(seq 1 300); do
        ! kill -0 $MPID 2>/dev/null && break
        sleep 1
    done
    rm -f "$WINEPREFIX_DIR/drive_d/$msi_file"
}
init_wineprefix() {
    if [ -f "$WINEPREFIX_DIR/drive_d/windows/system32/kernel32.dll" ]; then
        log "Wineprefix 已存在，跳过 wineboot"
        if [ ! -L "$WINEPREFIX_DIR/drive_c" ]; then
            log "修复 drive_c -> drive_d 符号链接..."
            rm -f "$WINEPREFIX_DIR/drive_c"
            ln -s drive_d "$WINEPREFIX_DIR/drive_c"
        fi
        return 0
    fi
    log "步骤 1/5: 运行 wineboot 初始化 (约 1-3 分钟)..."
    $BOX64 $WINE_BIN wineboot --init 2>/dev/null &
    local WPID=$!
    for i in $(seq 1 60); do
        if [ -d "$WINEPREFIX_DIR/drive_d" ] && [ ! -e "$WINEPREFIX_DIR/drive_c" ]; then
            ln -s drive_d "$WINEPREFIX_DIR/drive_c"
            log "drive_c -> drive_d 符号链接已创建"
            break
        fi
        sleep 1
    done
    for i in $(seq 1 180); do
        ! kill -0 $WPID 2>/dev/null && break
        sleep 1
    done
    kill_all_wine
    rm -f "$WINEPREFIX_DIR/dosdevices/z:"
    ln -s "$HOME" "$WINEPREFIX_DIR/dosdevices/z:"
    log "wineboot 完成"
}
install_dxvk() {
    if [ -f "$WINEPREFIX_DIR/drive_d/windows/system32/d3d11.dll" ] && grep -q "d3d11\"=\"native,builtin" "$WINEPREFIX_DIR/user.reg" 2>/dev/null; then
        log "DXVK 已安装，跳过"
        return 0
    fi
    if [ ! -d "$DXVK_DIR" ]; then
        warn "DXVK 目录不存在: $DXVK_DIR，跳过"
        return 0
    fi
    log "步骤 2/5: 安装 DXVK 2.5.3..."
    cp "$DXVK_DIR/d3d11.dll" "$WINEPREFIX_DIR/drive_d/windows/system32/"
    cp "$DXVK_DIR/dxgi.dll" "$WINEPREFIX_DIR/drive_d/windows/system32/"
    if ! grep -q "d3d11\"=\"native,builtin" "$WINEPREFIX_DIR/user.reg" 2>/dev/null; then
        printf n[Software\\Wine\\DllOverrides]nd3d11=native,builtinndxgi=native,builtinn >> "$WINEPREFIX_DIR/user.reg"
    fi
    log "DXVK 安装完成"
}
install_mono() {
    if [ -d "$WINEPREFIX_DIR/drive_d/windows/mono" ]; then
        log "Wine Mono 已安装，跳过"
        return 0
    fi
    if [ ! -f "$MONO_MSI" ]; then
        warn "Wine Mono 安装包不存在: $MONO_MSI，跳过"
        return 0
    fi
    log "步骤 3/5: 安装 Wine Mono 11.1.0 (约 1-2 分钟)..."
    kill_all_wine
    start_wineserver 0
    install_msi "$MONO_MSI"
    kill_all_wine
    log "Wine Mono 安装完成"
}
install_gecko() {
    if [ -d "$WINEPREFIX_DIR/drive_d/windows/system32/gecko" ] && ls "$WINEPREFIX_DIR/drive_d/windows/system32/gecko/"* &>/dev/null 2>/dev/null; then
        local gfiles=$(find "$WINEPREFIX_DIR/drive_d/windows/system32/gecko" -type f 2>/dev/null | wc -l)
        if [ "$gfiles" -gt 2 ]; then
            log "Wine Gecko 已安装，跳过"
            return 0
        fi
    fi
    kill_all_wine
    if [ -f "$GECKO_MSI_X64" ]; then
        log "步骤 4/5: 安装 Wine Gecko 2.47.4 x86_64 (约 1-2 分钟)..."
        start_wineserver 0
        install_msi "$GECKO_MSI_X64"
        kill_all_wine
    fi
    if [ -f "$GECKO_MSI_X86" ]; then
        log "步骤 4/5: 安装 Wine Gecko 2.47.4 x86 (约 1-2 分钟)..."
        start_wineserver 0
        install_msi "$GECKO_MSI_X86"
        kill_all_wine
    fi
    if [ -d "$WINEPREFIX_DIR/drive_d/windows/system32/gecko" ]; then
        log "Wine Gecko 安装完成"
    else
        warn "Wine Gecko 安装失败或安装包不存在，跳过"
    fi
}
enable_esync() {
    log "启用 ESYNC..."
    kill_all_wine
    WINEESYNC=1 start_wineserver 1
    log "ESYNC 已启用"
}
verify() {
    log "步骤 5/5: 验证安装..."
    local ok=true
    if [ -L "$WINEPREFIX_DIR/drive_c" ]; then
        log "  drive_c -> drive_d: OK"
    else
        warn "  drive_c 符号链接: 缺失!"
        ok=false
    fi
    if [ -f "$WINEPREFIX_DIR/drive_d/windows/system32/kernel32.dll" ]; then
        log "  Wineprefix: OK"
    else
        warn "  Wineprefix: 未初始化!"
        ok=false
    fi
    if [ -f "$WINEPREFIX_DIR/drive_d/windows/system32/d3d11.dll" ]; then
        log "  DXVK: OK"
    else
        warn "  DXVK: 未安装"
        ok=false
    fi
    if [ -d "$WINEPREFIX_DIR/drive_d/windows/mono" ]; then
        log "  Wine Mono: OK"
    else
        warn "  Wine Mono: 未安装"
        ok=false
    fi
    if [ -d "$WINEPREFIX_DIR/drive_d/windows/system32/gecko" ] && ls "$WINEPREFIX_DIR/drive_d/windows/system32/gecko/"* &>/dev/null 2>/dev/null; then
        local gfiles=$(find "$WINEPREFIX_DIR/drive_d/windows/system32/gecko" -type f 2>/dev/null | wc -l)
        if [ "$gfiles" -gt 2 ]; then
            log "  Wine Gecko: OK"
        else
            warn "  Wine Gecko: 未安装"
            ok=false
        fi
    else
        warn "  Wine Gecko: 未安装"
        ok=false
    fi
    if $ok; then
        log ""
        log "初始化完成! 现在可以启动游戏:"
        log "  ~/proton11/proton11-run /path/to/game.exe"
    else
        die "初始化未完成，请检查上述错误"
    fi
}
install_fonts() {
    echo ">>> 安装/修复游戏字体 (彻底方案)..."
    echo "    改名字体 + 双 Fonts 键直接登记, 绕开 FontSubstitutes 递归"
    kill_all_wine
    local FONTDIR="$WINEPREFIX_DIR/drive_d/windows/Fonts"
    local REG="$WINEPREFIX_DIR/system.reg"
    local UREG="$WINEPREFIX_DIR/user.reg"
    [ -d "$FONTDIR" ] || { warn "字体目录缺失: $FONTDIR, 跳过字体安装"; return 0; }
    [ -f "$REG" ] || { warn "注册表缺失: $REG, 跳过字体安装"; return 0; }

    # --- 1. 字体文件缺失则尝试从源字体生成 (需 FONT_SRC 含 IPA 提取目录 + Noto CJK ttc) ---
    local missing=0
    for f in SourceHanSansSC-Bold.otf msgothic.ttf msgothicp.ttf msuigothic.ttf msmincho.ttf msminchop.ttf tahoma.ttf tahomabd.ttf yugothic.ttf yugothui.ttf meiryo.ttf meiryoui.ttf malgun.ttf msyh.ttf msyhui.ttf simsun.ttf simhei.ttf mingliu.ttf pmingliu.ttf msjh.ttf msjhui.ttf segoeui.ttf; do
        [ -f "$FONTDIR/$f" ] || missing=1
    done
    if [ "$missing" -eq 1 ]; then
        if [ -d "$FONT_SRC" ]; then
            log "检测到字体缺失, 尝试从 $FONT_SRC 生成改名字体..."
            # Gothic 系列从 wqy (CJK 完备, 中文不缺字) 改名; Mincho 系列从 IPA 生成
            python3 "$FONT_FIX/mkmsfonts_cjk.py" 2>&1 | tail -5 || warn "mkmsfonts_cjk.py 失败"
            python3 "$FONT_FIX/mkmsfonts.py" 2>&1 | tail -5 || warn "mkmsfonts.py 失败 (检查 $FONT_SRC 布局)"
            # Tahoma 系列从 wqy 改名成内部名=Tahoma (中文完备, 让 family Tahoma 带 CJK 字形)
            python3 "$FONT_FIX/mkmsfonts_tahoma.py" 2>&1 | tail -5 || warn "mkmsfonts_tahoma.py 失败"
            # 广覆盖: 把 wqy 改名成一大批常见 CJK 家族名(Yu Gothic/Meiryo/Microsoft YaHei/
            # SimSun/Malgun/MingLiU/Segoe UI ...), 仅依赖 wqy, 无需 FONT_SRC
            python3 "$FONT_FIX/mkmsfonts_more.py" 2>&1 | tail -5 || warn "mkmsfonts_more.py 失败"
            local ttc
            ttc=$(ls "$FONT_SRC"/NotoSansCJK*.ttc 2>/dev/null | head -1)
            if [ -n "$ttc" ]; then
                python3 "$FONT_FIX/mkhanfont.py" "$ttc" 2>&1 | tail -3 || warn "mkhanfont.py 失败"
            else
                warn "未找到 Noto CJK ttc, 跳过 SourceHanSansSC-Bold 生成"
            fi
            [ -d "$HOME/jpfont/out" ] && cp -f "$HOME/jpfont/out/"*.ttf "$FONTDIR/" 2>/dev/null
            [ -f "$HOME/jpfont/out/SourceHanSansSC-Bold.otf" ] && cp -f "$HOME/jpfont/out/SourceHanSansSC-Bold.otf" "$FONTDIR/"
        else
            warn "字体文件缺失且 FONT_SRC 未设置, 跳过重生成 (依赖已装字体 / .good 快照)"
        fi
    else
        log "6 个字体均已在 prefix, 直接进入注册表登记"
    fi

    # --- 2. 写注册表: 双 Fonts 键直接登记 + 移除 MS* 替换 + 清 Wine 字体缓存 ---
    backup_reg "$REG"
    backup_reg "$UREG"
    python3 - "$REG" "$UREG" <<'PY'
import sys, re
reg, ureg = sys.argv[1], sys.argv[2]
# 注意: 注册表文件里反斜杠是转义的 "\\", 键名必须用双反斜杠才能匹配
TARGETS = [
    ("MS Gothic",            "msgothic.ttf",          "TrueType", False),
    ("MS PGothic",           "msgothicp.ttf",         "TrueType", False),
    ("MS UI Gothic",         "msuigothic.ttf",        "TrueType", False),
    ("MS Mincho",            "msmincho.ttf",          "TrueType", False),
    ("MS PMincho",           "msminchop.ttf",         "TrueType", False),
    ("SourceHanSansSC-Bold", "SourceHanSansSC-Bold.otf", "OpenType", True),
    # 糖调(Sugar☆Full Tempering) 等 BGI 游戏直接用 Tahoma 渲染中文.
    # 关键: Wine 自带 bundled tahoma.ttf 只有拉丁字形 -> 中文全豆腐.
    # 必须把 wqy 改名成内部名=Tahoma 的 tahoma.ttf(既中文完备, 又让 family "Tahoma"
    # 真正携带 CJK 字形), 再登记到 Fonts 键. 直接 "Tahoma (TrueType)"="wqy-microhei.ttc"
    # 无效——wqy 内部名是 WenQuanYi, 会归到别的 family, 裸名 Tahoma 仍命中 bundled 拉丁版.
    ("Tahoma",               "tahoma.ttf",            "TrueType", False),
    ("Tahoma Bold",          "tahomabd.ttf",          "TrueType", False),
]
FONTS_KEYS = [
    r'[Software\\Microsoft\\Windows\\CurrentVersion\\Fonts]',
    r'[Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts]',
]
SUB_KEY = r'[Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes]'
KILL_SUB = {"MS Gothic", "MS PGothic", "MS UI Gothic", "MS Mincho", "MS PMincho"}
# 广覆盖: 常见东亚/中文字体名 -> WenQuanYi Micro Hei (中文完备). 这些名多数没有 bundled
# 阴影字体, 用 FontSubstitutes 替换即可兜底; 少数已生成真实改名文件者(见下)替换不触发也无妨.
# 注意: 绝不列符号字体(marlett/symbol/webdings/wingdings)与纯拉丁字体(arial/times...),
# 那些改掉会破坏 UI 布局/符号——默认注册表已把 arial/courier 等映射到 WenQuanYi, 这里不动.
EXTRA_SUB = [
    # 韩文
    "Gulim","GulimChe","Batang","BatangChe","Gungsuh","GungsuhChe",
    # 日文衬线 / macOS 系
    "Yu Mincho","Yu Mincho Light","Hiragino Kaku Gothic Pro","Hiragino Kaku Gothic ProN",
    "Hiragino Sans","Hiragino Mincho ProN","Hiragino Sans GB","Hiragino Sans CNS",
    # 简体中文(书法/方正/华文等)
    "KaiTi","KaiTi_GB2312","FangSong","FangSong_GB2312","LiSu","YouYuan","DengXian",
    "STSong","STHeiti","STKaiti","STXingkai","STLiti","STZhongsong","STFangsong",
    "STXihei","STXinwei","STKaitiSC","STSongtiSC","STYuanti","FZShuTi","FZKai-Z03",
    # 繁体中文
    "DFKai-SB","KaiU","MingLiU_HKSCS","PMingLiU_HKSCS",
    # 字重变体
    "Microsoft YaHei Light",
    # 已生成真实改名文件的 UI 变体(双保险)
    "Yu Gothic UI","Meiryo UI","Malgun Gothic","Microsoft YaHei UI",
    "MingLiU","PMingLiU","Microsoft JhengHei","Microsoft JhengHei UI",
]
# 完整值键名集合 (值键形如 "MS Gothic (TrueType)"), 用于去重
KEYS = {f"{disp} ({suffix})" for (disp, _f, suffix, _o) in TARGETS}

def parse(lines):
    sections, order, cur = {}, [], None
    for ln in lines:
        s = ln.rstrip("\r\n")
        # Wine 节头形如 [Key] 1234567890 (末尾有时间戳), 不能要求以 ] 结尾
        if s.startswith("[") and "]" in s:
            cur = s[:s.index("]")+1]
            if cur not in sections:
                sections[cur] = []; order.append(cur)
        elif cur is not None:
            sections[cur].append(ln)
        else:
            sections.setdefault("__pre__", []).append(ln)
            if "__pre__" not in order: order.append("__pre__")
    return sections, order

def dump(sections, order):
    out = []
    for sec in order:
        if sec != "__pre__":
            out.append(sec + "\n")
        out.extend(sections[sec])
    return out

def read(p):
    with open(p, encoding="utf-8", errors="surrogateescape") as f: return f.readlines()
def write(p, lines):
    with open(p, "w", encoding="utf-8", errors="surrogateescape") as f: f.writelines(lines)

# ---- system.reg: 双 Fonts 键写入 6 个直接登记项 (原位修改, 不追加) ----
sec, order = parse(read(reg))
for key in FONTS_KEYS:
    if key not in sec:
        sec[key] = []; order.append(key)
    body = sec[key]
    kept = []
    for ln in body:
        s = ln.rstrip("\r\n")
        m = re.match(r'"([^"]+)"=', s)
        if m and m.group(1) in KEYS:
            continue   # 丢弃已有同名值键, 下面用正确值重写
        kept.append(ln)
    for (disp, fname, suffix, _o) in TARGETS:
        kept.append('"%s (%s)"="%s"\n' % (disp, suffix, fname))
    sec[key] = kept
# ---- 移除 FontSubstitutes 中的 MS* 项 (避免回报名不匹配递归) ----
if SUB_KEY in sec:
    newbody = []
    for ln in sec[SUB_KEY]:
        s = ln.rstrip("\r\n")
        m = re.match(r'"([^"]+)"=', s)
        if m and m.group(1) in KILL_SUB:
            continue
        newbody.append(ln)
    sec[SUB_KEY] = newbody
# ---- 广覆盖: 追加 EXTRA_SUB -> WenQuanYi Micro Hei (已存在则跳过, 幂等) ----
if SUB_KEY in sec:
    have = set()
    for ln in sec[SUB_KEY]:
        m = re.match(r'"([^"]+)"=', ln.rstrip("\r\n"))
        if m: have.add(m.group(1))
    for name in EXTRA_SUB:
        if name not in have:
            sec[SUB_KEY].append('"%s"="WenQuanYi Micro Hei"\n' % name)
write(reg, dump(sec, order))

# ---- user.reg: 清除 Wine 字体缓存节, 强制重扫 Fonts 目录 ----
sec_u, order_u = parse(read(ureg))
neworder = [s for s in order_u if not s.startswith(r'[Software\\Wine\\Fonts')]
sec_u = {s: v for s, v in sec_u.items() if s in neworder}
write(ureg, dump(sec_u, neworder))
print(">>> 注册表已写入: 双 Fonts 键登记 6 字体 + 清除 MS* 替换 + 清 Wine 字体缓存")
PY

    # --- 3. 校验 ---
    local ok=0
    for f in SourceHanSansSC-Bold.otf msgothic.ttf msgothicp.ttf msuigothic.ttf msmincho.ttf msminchop.ttf tahoma.ttf tahomabd.ttf yugothic.ttf yugothui.ttf meiryo.ttf meiryoui.ttf malgun.ttf msyh.ttf msyhui.ttf simsun.ttf simhei.ttf mingliu.ttf pmingliu.ttf msjh.ttf msjhui.ttf segoeui.ttf; do
        [ -f "$FONTDIR/$f" ] && ok=$((ok+1))
    done
    if [ "$ok" -eq 22 ]; then
        log "字体文件 22/22 就位; 注册表登记完成"
    else
        warn "字体文件仅 $ok/22 就位 (若缺源字体需先准备 FONT_SRC)"
    fi
}
main() {
    log "Proton 11 环境初始化"
    log ""
    check_prereqs
    setup_env
    init_wineprefix
    install_dxvk
    install_mono
    install_gecko
    install_fonts
    enable_esync
    verify
}
main "$@"
