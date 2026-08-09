#!/usr/bin/env python3
"""
从 Noto Sans CJK Bold 抽出 SC face, 改名为 "SourceHanSansSC-Bold",
供 magiccharming 的汉化补丁使用。

为什么需要:
  +font 跟踪显示游戏启动时依次用 charset 128/136/134/0 枚举
  L"SourceHanSansSC-Bold", 四次全部落空 (无 enum_face_charsets 输出)。
  随后 fallback 到系统默认 UI 字体 L"Segoe UI", 而 FontSubstitutes 把它
  换成 WenQuanYi Micro Hei, 于是 0x432760 里的 wcscmp 永远不等,
  在 0x4329c5 无限递归 -> STATUS_STACK_OVERFLOW (0xc00000fd) x13425。

为什么用 Noto Sans CJK:
  Noto Sans CJK 与 Adobe Source Han Sans 是同一套字体的两个发行名,
  字形完全一致。OS/2 的 ulCodePageRange 保持原样不动, 这样
  charset 探测顺序与真 Windows 上装了 SourceHanSansSC-Bold.otf 时一致。
"""
import os
import sys
from fontTools.ttLib import TTCollection, TTFont

TARGET_FAMILY = "SourceHanSansSC-Bold"
SRC_FACE = "Noto Sans CJK SC"          # Bold ttc 里 face 名不带权重后缀
OUT = os.path.expanduser("~/jpfont/out")

WIN_EN, MAC_EN = (3, 1, 0x409), (1, 0, 0)


def pick_face(ttc_path):
    ttc = TTCollection(ttc_path, lazy=False)
    for i, f in enumerate(ttc.fonts):
        fam = f["name"].getName(1, 3, 1, 0x409)
        if fam and str(fam).startswith(SRC_FACE):
            return i, f
    raise SystemExit(f"在 {ttc_path} 里找不到 {SRC_FACE}, 现有: "
                     + ", ".join(str(x["name"].getName(1, 3, 1, 0x409)) for x in ttc.fonts))


def main():
    if len(sys.argv) < 2:
        raise SystemExit("用法: mkhanfont.py <NotoSansCJK-Bold.ttc>")
    ttc_path = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)

    idx, font = pick_face(ttc_path)
    os2 = font["OS/2"]
    print(f">>> 取 face[{idx}] weight={os2.usWeightClass} "
          f"cp1=0x{os2.ulCodePageRange1:08x} (保持不变)")

    name = font["name"]
    name.names = []

    def setn(nid, value, *targets):
        for plat, enc, lang in targets:
            name.setName(value, nid, plat, enc, lang)

    setn(1, TARGET_FAMILY, WIN_EN, MAC_EN)   # 家族名 —— 游戏 wcscmp 比这个
    setn(2, "Regular", WIN_EN, MAC_EN)
    setn(4, TARGET_FAMILY, WIN_EN, MAC_EN)   # 完整名 —— 也比这个
    setn(6, TARGET_FAMILY, WIN_EN, MAC_EN)
    setn(3, f"{TARGET_FAMILY}:NotoSansCJK", WIN_EN, MAC_EN)
    setn(5, "Version 2.004", WIN_EN, MAC_EN)
    setn(0, "Licensed under the SIL Open Font License 1.1. "
            "Renamed from Noto Sans CJK SC Bold for application compatibility.",
         WIN_EN, MAC_EN)
    setn(13, "SIL Open Font License 1.1", WIN_EN)
    setn(14, "https://scripts.sil.org/OFL", WIN_EN)

    for nid in (16, 17, 21, 22):
        for rec in [r for r in name.names if r.nameID == nid]:
            name.names.remove(rec)

    ext = ".otf" if "CFF " in font else ".ttf"
    out = os.path.join(OUT, TARGET_FAMILY + ext)
    font.save(out)
    font.close()

    # 复核
    f = TTFont(out, lazy=True)
    n = f["name"]
    fam = str(n.getName(1, 3, 1, 0x409))
    full = str(n.getName(4, 3, 1, 0x409))
    stray = sorted({r.nameID for r in n.names if r.nameID in (16, 17, 21, 22)})
    cp1 = f["OS/2"].ulCodePageRange1
    f.close()

    print(f"[+] {os.path.basename(out)}  ({os.path.getsize(out)} bytes)")
    print(f"      nameID1={fam!r}  nameID4={full!r}")
    print(f"      残留16/17={stray or '无'}  cp1=0x{cp1:08x}")
    ok = fam == TARGET_FAMILY and full == TARGET_FAMILY and not stray
    print(">>> 校验通过" if ok else ">>> 校验失败")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
