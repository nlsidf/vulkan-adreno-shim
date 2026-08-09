#!/data/data/com.termux/files/usr/bin/bash
# 把 ~/jpfont/out/ 里改名好的 MS Gothic / MS Mincho 装进 Wine prefix,
# 并移除会导致 magicha.exe 字体校验递归爆栈 (0xc00000fd) 的 FontSubstitutes 映射。
#
# 背景: magicha.exe 0x432760 会用 wcscmp 校验 GetOutlineTextMetricsW
# 返回的 otmpFaceName 是否等于请求名 ("MS Gothic"/"MS Mincho")。
# FontSubstitutes 让 Wine 返回 "WenQuanYi Micro Hei", 校验永远失败,
# 函数在 0x4329c5 用同样参数无限自调用 -> 8787 层 -> 爆栈。
#
# 字体来源: IPAfont00303 (IPA Font License v1.0), name table 已改名。
# 生成脚本: ~/proton11/font-fix/mkmsfonts.py

set -euo pipefail

OUTDIR="$HOME/jpfont/out"
PREFIX="$HOME/proton11/p11prefix"
FONTDIR="$PREFIX/drive_d/windows/Fonts"
REG="$PREFIX/system.reg"
USERREG="$PREFIX/user.reg"

FONTS=(msgothic.ttf msgothicp.ttf msuigothic.ttf msmincho.ttf msminchop.ttf)

die() { echo "错误: $*" >&2; exit 1; }

# --- 1. 检查产物 ---
for f in "${FONTS[@]}"; do
    [ -f "$OUTDIR/$f" ] || die "缺少 $OUTDIR/$f, 请先跑: python3 ~/proton11/font-fix/mkmsfonts.py"
done
[ -d "$FONTDIR" ] || die "找不到 prefix 字体目录: $FONTDIR"
[ -f "$REG" ]     || die "找不到注册表: $REG"

# --- 2. 停掉 wineserver (它退出时会覆写注册表) ---
if pgrep -x wineserver >/dev/null 2>&1; then
    echo ">>> wineserver 在跑, 强制结束 (ESYNC 下 wineserver -k 会静默失败)"
    kill -9 $(pgrep wineserver) 2>/dev/null || true
    sleep 1
fi

# --- 3. 安装字体 ---
echo ">>> 安装字体到 $FONTDIR"
for f in "${FONTS[@]}"; do
    cp -f "$OUTDIR/$f" "$FONTDIR/$f"
    printf '    %-16s %s bytes\n' "$f" "$(stat -c%s "$FONTDIR/$f")"
done

# --- 4. 移除 FontSubstitutes 映射 ---
BAK="$REG.bak-msfonts-$(date +%Y%m%d%H%M%S)"
cp -a "$REG" "$BAK"
echo ">>> 注册表已备份: $BAK"

python3 - "$REG" <<'PY'
import sys, re

path = sys.argv[1]
# 这些名字必须让 Wine 解析到我们刚装的真字体, 不能被 substitute 掉
KILL = {"MS Gothic", "MS PGothic", "MS UI Gothic", "MS Mincho", "MS PMincho"}
SECTION = r'[Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes]'

with open(path, encoding="utf-8", errors="surrogateescape") as fh:
    lines = fh.readlines()

out, in_sec, removed = [], False, []
for line in lines:
    stripped = line.rstrip("\r\n")
    if stripped.startswith("["):
        in_sec = stripped.startswith(SECTION)
    if in_sec:
        m = re.match(r'"([^"]+)"=', stripped)
        if m and m.group(1) in KILL:
            removed.append(stripped)
            continue
    out.append(line)

with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
    fh.writelines(out)

if removed:
    print(">>> 已删除以下替换规则:")
    for r in removed:
        print("    " + r)
else:
    print(">>> 未找到需要删除的替换规则 (可能已处理过)")
PY

# --- 5. 清掉 Wine 字体缓存, 强制重扫 Fonts 目录 ---
if grep -q 'Software\\\\Wine\\\\Fonts' "$USERREG" 2>/dev/null; then
    cp -a "$USERREG" "$USERREG.bak-msfonts-$(date +%Y%m%d%H%M%S)"
    python3 - "$USERREG" <<'PY'
import sys
path = sys.argv[1]
with open(path, encoding="utf-8", errors="surrogateescape") as fh:
    lines = fh.readlines()
out, skip, n = [], False, 0
for line in lines:
    s = line.rstrip("\r\n")
    if s.startswith("["):
        skip = s.startswith(r'[Software\\Wine\\Fonts')
        if skip:
            n += 1
    if not skip:
        out.append(line)
with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
    fh.writelines(out)
print(f">>> 已清除 {n} 个 Wine 字体缓存节, 下次启动会重扫 Fonts 目录")
PY
else
    echo ">>> 无 Wine 字体缓存节, 跳过"
fi

echo
echo ">>> 完成。验证:"
echo "      ~/proton11/font-fix/run-dbg.sh"
echo "      grep -c 'code=c00000fd' ~/proton11/.build/magiccharming-dbg.log"
echo "    修复成功应为 0 (修复前是 13427)。"
