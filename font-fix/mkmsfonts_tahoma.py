#!/usr/bin/env python3
"""
把 WenQuanYi Micro Hei (wqy-microhei.ttc, face 0) 改名成 Tahoma / Tahoma Bold 系列,
写入 prefix 的 Fonts 目录。糖调(Sugar☆Full Tempering) 直接用 Tahoma 渲染文本,
而 Wine 自带的 bundled tahoma.ttf 只有拉丁字形 -> 中文全变豆腐块。

关键点(同 Amairo 的 MS Gothic 修复): 字体**内部 family name 必须字面等于 "Tahoma"**,
否则 Wine 仍会用 bundled 的拉丁版 tahoma.ttf 来应答 Tahoma 请求。这里把 wqy 改名后,
既保留中文完备 cmap, 又让内部名 == 请求名, 从而让 family "Tahoma" 真正携带 CJK 字形。

粗体: Tahoma Bold 单独生成一个 subfamily=Bold 的 face, 与 bundled 拉丁粗体共存,
Wine 按字形回退时选中带 CJK 的那个。
"""
import os
import sys
from fontTools.ttLib import TTFont

WQY = os.path.expanduser("~/proton11/p11prefix/drive_d/windows/Fonts/wqy-microhei.ttc")
OUT = os.path.expanduser("~/proton11/p11prefix/drive_d/windows/Fonts")

# (输出文件, family, full, postscript, subfamily)
JOBS = [
    ("tahoma.ttf",   "Tahoma",      "Tahoma",      "Tahoma",      "Regular"),
    ("tahomabd.ttf", "Tahoma",      "Tahoma Bold", "Tahoma-Bold", "Bold"),
]

WIN_EN, WIN_JA, MAC_EN = (3, 1, 0x409), (3, 1, 0x411), (1, 0, 0)


def set_names(font, family, full, ps, subfamily):
    name = font["name"]
    keep = []
    for rec in name.names:
        if rec.nameID in (1, 2, 4, 6) and rec.platformID == 3 and rec.langID in (0x409, 0x411):
            continue
        if rec.nameID in (1, 2, 4, 6) and rec.platformID == 1 and rec.langID == 0:
            continue
        keep.append(rec)
    name.names = keep

    def add(pid, enc, lid, nid, val):
        name.setName(val, nid, pid, enc, lid)

    for (pid, enc, lid) in (WIN_EN, WIN_JA, MAC_EN):
        add(pid, enc, lid, 1, family)
        add(pid, enc, lid, 2, subfamily)
        add(pid, enc, lid, 4, full)
        add(pid, enc, lid, 6, ps)


def main():
    if not os.path.exists(WQY):
        sys.exit(f"wqy 源字体不存在: {WQY}")
    for out_name, family, full, ps, sub in JOBS:
        font = TTFont(WQY, fontNumber=0, lazy=False)
        set_names(font, family, full, ps, sub)
        dst = os.path.join(OUT, out_name)
        font.save(dst)
        font.close()
        print(f"wrote {dst}  (family={family!r}, full={full!r}, sub={sub!r})")


if __name__ == "__main__":
    main()
