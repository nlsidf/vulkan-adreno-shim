#!/usr/bin/env python3
"""
把 WenQuanYi Micro Hei (wqy-microhei.ttc, face 0) 改名成一大批常见 CJK 字体家族名,
写入 prefix 的 Fonts 目录, 做"广覆盖"兜底。

适用场景: 许多 BGI / AVG / 国产游戏会请求各种东亚字体名
(Yu Gothic / Meiryo / Microsoft YaHei / SimSun / Malgun Gothic / MingLiU / Segoe UI ...).
只要游戏请求的家族名 == 字体内部名, Wine 就能用这个中文完备的 wqy 字形去渲染,
不会掉进拉丁版 bundled 字体(只有 Tahoma 系被 bundled 拉丁版阴影覆盖, 已单独在
mkmsfonts_tahoma.py 处理)或 FontSubstitutes 替换不触发导致的豆腐坑。

关键(同前): 内部 family name 必须字面等于游戏请求的家族名, 否则归到别的 family,
裸名请求仍可能命中无中文字形的字体。

这里只生成 Regular(子族 Regular); 粗体由 Wine 对 TrueType 轮廓做合成粗体(faux bold)完成,
wqy 是 TTF 轮廓, 合成粗体可用, 字形覆盖与 Regular 完全一致。

注意: 绝不处理符号字体(marlett/symbol/webdings/wingdings)与纯拉丁字体(arial/times...),
那些改掉会破坏 UI 布局/符号。本列表全是承载 CJK 文本的东亚/UI 字体。
"""
import os
import sys
from fontTools.ttLib import TTFont

WQY = os.path.expanduser("~/proton11/p11prefix/drive_d/windows/Fonts/wqy-microhei.ttc")
OUT = os.path.expanduser("~/proton11/p11prefix/drive_d/windows/Fonts")

# (输出文件, 英文family, PostScript名)
# 子族统一 Regular; full name == family。
JOBS = [
    # 日文 UI / 系统字体
    ("yugothic.ttf",  "Yu Gothic",        "YuGothic"),
    ("yugothui.ttf",  "Yu Gothic UI",     "YuGothicUI"),
    ("meiryo.ttf",    "Meiryo",           "Meiryo"),
    ("meiryoui.ttf",  "Meiryo UI",        "MeiryoUI"),
    # 韩文
    ("malgun.ttf",    "Malgun Gothic",    "MalgunGothic"),
    # 简体中文
    ("msyh.ttf",      "Microsoft YaHei",      "MicrosoftYaHei"),
    ("msyhui.ttf",    "Microsoft YaHei UI",  "MicrosoftYaHeiUI"),
    ("simsun.ttf",    "SimSun",           "SimSun"),
    ("simhei.ttf",    "SimHei",           "SimHei"),
    # 繁体中文
    ("mingliu.ttf",   "MingLiU",          "MingLiU"),
    ("pmingliu.ttf",  "PMingLiU",         "PMingLiU"),
    ("msjh.ttf",      "Microsoft JhengHei",      "MicrosoftJhengHei"),
    ("msjhui.ttf",    "Microsoft JhengHei UI",   "MicrosoftJhengHeiUI"),
    # 通用 UI(Windows 大量使用, 携带 CJK 时也需要中文完备)
    ("segoeui.ttf",   "Segoe UI",         "SegoeUI"),
]

WIN_EN, WIN_JA, MAC_EN = (3, 1, 0x409), (3, 1, 0x411), (1, 0, 0)


def set_names(font, family, ps):
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
        add(pid, enc, lid, 2, "Regular")
        add(pid, enc, lid, 4, family)
        add(pid, enc, lid, 6, ps)


def main():
    if not os.path.exists(WQY):
        sys.exit(f"wqy 源字体不存在: {WQY}")
    for out_name, family, ps in JOBS:
        font = TTFont(WQY, fontNumber=0, lazy=False)
        set_names(font, family, ps)
        dst = os.path.join(OUT, out_name)
        font.save(dst)
        font.close()
        print(f"wrote {dst}  (family={family!r}, ps={ps!r})")


if __name__ == "__main__":
    main()
