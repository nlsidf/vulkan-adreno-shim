#!/usr/bin/env python3
"""
把 WenQuanYi Micro Hei (wqy-microhei.ttc, face 0) 改名成 MS Gothic / MS PGothic /
MS UI Gothic 系列, 写入 prefix 的 Fonts 目录。

为什么用 wqy 而不是 IPA:
  wqy 是完整 CJK 字体, 覆盖 GB2312/GBK + 日文假名汉字; IPA 仅覆盖 JIS (X 0208),
  简体专用字 (如 "气", JIS 只有 "氣") 会缺字变成豆腐块。
  巧克甜恋(BGI 汉化版) 用 MS Gothic 渲染中文, 因此 MS Gothic 必须是中文完备字体。

为什么仍要改名 (关键):
  Magical Charming 的字体加载函数拿请求名跟字体内部名做 wcscmp, 不等则无限递归
  (STATUS_STACK_OVERFLOW)。所以内部 family name 必须字面等于 "MS Gothic" 等。
  这里把 wqy 改名后, 既保留中文完备性, 又让 Magical Charming 的 wcscmp 命中不崩溃。

Mincho 系列 (MS Mincho / MS PMincho) 保持 IPA 衬线日文字体, 不参与中文渲染,
  故本脚本不处理 (由 mkmsfonts.py 生成)。
"""
import os
import sys
from fontTools.ttLib import TTFont

WQY = os.path.expanduser("~/proton11/p11prefix/drive_d/windows/Fonts/wqy-microhei.ttc")
OUT = os.path.expanduser("~/proton11/p11prefix/drive_d/windows/Fonts")

# (输出文件, 英文family, 日文family, PostScript名)
JOBS = [
    ("msgothic.ttf",   "MS Gothic",    "\uff2d\uff33 \u30b4\u30b7\u30c3\u30af",             "MSGothic"),
    ("msgothicp.ttf",  "MS PGothic",   "\uff2d\uff33 \uff30\u30b4\u30b7\u30c3\u30af",       "MSPGothic"),
    ("msuigothic.ttf", "MS UI Gothic", "\uff2d\uff33 \uff35\uff29\u30b4\u30b7\u30c3\u30af", "MSUIGothic"),
]

WIN_EN, WIN_JA, MAC_EN = (3, 1, 0x409), (3, 1, 0x411), (1, 0, 0)


def set_names(font, en_family, ja_family, ps):
    name = font["name"]
    # 先清掉与本次目标冲突的记录, 再写入
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

    # family (1), subfamily (2), full (4), postscript (6) 三平台
    for (pid, enc, lid) in (WIN_EN, WIN_JA, MAC_EN):
        add(pid, enc, lid, 1, en_family)
        add(pid, enc, lid, 2, "Regular")
        add(pid, enc, lid, 4, en_family)
        add(pid, enc, lid, 6, ps)
    # 日文 family 单独写 (nameID 1, platform 3, lang 0x411)
    name.setName(ja_family, 1, 3, 1, 0x411)
    name.setName(ja_family, 4, 3, 1, 0x411)


def main():
    if not os.path.exists(WQY):
        sys.exit(f"wqy 源字体不存在: {WQY}")
    for out_name, en, ja, ps in JOBS:
        font = TTFont(WQY, fontNumber=0, lazy=False)
        # 清掉 wqy 自带的 @-variant / 多余 name, 避免干扰
        set_names(font, en, ja, ps)
        dst = os.path.join(OUT, out_name)
        font.save(dst)
        font.close()
        print(f"wrote {dst}  (family={en!r}, ja={ja!r})")


if __name__ == "__main__":
    main()
