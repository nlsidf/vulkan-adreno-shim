#!/usr/bin/env python3
"""
把 IPA 日文字体改名成 MS Gothic / MS Mincho 系列, 供 magicha.exe (BGI 引擎) 使用。

为什么必须改名:
  magicha.exe 0x432760 的字体加载函数会拿请求的字体名跟
  GetOutlineTextMetricsW 返回的 otmpFaceName / otmpFamilyName 做 wcscmp
  (0x432864 / 0x4328b0)。不相等就在 0x4329c5 用同样参数递归自调用,
  8787 层后 STATUS_STACK_OVERFLOW (0xc00000fd) 进程猝死。
  所以字体的内部 face name 必须字面等于 "MS Gothic" / "MS Mincho"。

为什么同时写英语和日语名:
  游戏先调 IsJapaneseLocale() (0x47a930, 检查 LANGID & 0x3ff == 0x11),
  日文 locale 下请求 cp932 全角名 "ＭＳ ゴシック", 否则请求 ASCII "MS Gothic"。
  两份 name 记录都写上, 将来切日文 locale 也不会退化。
"""
import os
import sys
from fontTools.ttLib import TTFont

SRC = os.path.expanduser("~/jpfont")
OUT = os.path.expanduser("~/jpfont/out")

IPAG  = f"{SRC}/x-gothic/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf"
IPAGP = f"{SRC}/x-gothic/usr/share/fonts/opentype/ipafont-gothic/ipagp.ttf"
IPAM  = f"{SRC}/x-mincho/usr/share/fonts/opentype/ipafont-mincho/ipam.ttf"
IPAMP = f"{SRC}/x-mincho/usr/share/fonts/opentype/ipafont-mincho/ipamp.ttf"

# (源文件, 输出名, 英文family, 日文family, PostScript名)
JOBS = [
    (IPAG,  "msgothic.ttf",   "MS Gothic",    "\uff2d\uff33 \u30b4\u30b7\u30c3\u30af",             "MSGothic"),
    (IPAGP, "msgothicp.ttf",  "MS PGothic",   "\uff2d\uff33 \uff30\u30b4\u30b7\u30c3\u30af",       "MSPGothic"),
    (IPAG,  "msuigothic.ttf", "MS UI Gothic", "\uff2d\uff33 \uff35\uff29\u30b4\u30b7\u30c3\u30af", "MSUIGothic"),
    (IPAM,  "msmincho.ttf",   "MS Mincho",    "\uff2d\uff33 \u660e\u671d",                         "MSMincho"),
    (IPAMP, "msminchop.ttf",  "MS PMincho",   "\uff2d\uff33 \uff30\u660e\u671d",                   "MSPMincho"),
]

# Windows平台(3,1) 英语 / 日语, 以及 Mac平台(1,0) 英语
WIN_EN, WIN_JA, MAC_EN = (3, 1, 0x409), (3, 1, 0x411), (1, 0, 0)


def retarget(src, outname, fam_en, fam_ja, psname):
    font = TTFont(src)
    name = font["name"]

    # 清空原有 name 记录, 从零重建, 避免 IPAGothic 的残留名字被 GDI 挑中
    name.names = []

    def setn(nid, value, *targets):
        for plat, enc, lang in targets:
            name.setName(value, nid, plat, enc, lang)

    # 1=家族名 2=子家族 4=完整名 —— 游戏 wcscmp 比的就是 1 和 4
    setn(1, fam_en, WIN_EN, MAC_EN)
    setn(1, fam_ja, WIN_JA)
    setn(2, "Regular", WIN_EN, WIN_JA, MAC_EN)
    setn(4, fam_en, WIN_EN, MAC_EN)
    setn(4, fam_ja, WIN_JA)
    setn(6, psname, WIN_EN, MAC_EN)          # PostScript 名, 不能含空格
    setn(3, f"{psname}:IPAfont00303", WIN_EN, MAC_EN)   # 唯一标识
    setn(5, "Version 003.03", WIN_EN, MAC_EN)
    # 保留 IPA 授权声明 (IPA Font License v1.0 要求保留)
    setn(0, "Licensed under the IPA Font License Agreement v1.0. "
            "Renamed from IPAfont00303 for application compatibility.",
         WIN_EN, MAC_EN)
    setn(13, "IPA Font License Agreement v1.0", WIN_EN)
    setn(14, "https://moji.or.jp/ipafont/license/", WIN_EN)

    # nameID 16/17 (Typographic Family) 会盖过 1/2, 必须确保不存在
    for nid in (16, 17, 21, 22):
        for rec in [r for r in name.names if r.nameID == nid]:
            name.names.remove(rec)

    out = os.path.join(OUT, outname)
    font.save(out)
    font.close()
    return out


def verify(path, fam_en, fam_ja):
    f = TTFont(path, lazy=True)
    n = f["name"]
    ok = True
    for nid in (1, 4):
        en = n.getName(nid, 3, 1, 0x409)
        ja = n.getName(nid, 3, 1, 0x411)
        en_s = str(en) if en else None
        ja_s = str(ja) if ja else None
        if en_s != fam_en or ja_s != fam_ja:
            ok = False
        print(f"      nameID{nid}: EN={en_s!r}  JA={ja_s!r}")
    stray = sorted({r.nameID for r in n.names if r.nameID in (16, 17, 21, 22)})
    if stray:
        ok = False
        print(f"      !! 残留 nameID {stray}")
    cp1 = f["OS/2"].ulCodePageRange1
    if not (cp1 >> 17) & 1:
        ok = False
        print("      !! OS/2 缺少 cp932 位")
    f.close()
    return ok


def main():
    os.makedirs(OUT, exist_ok=True)
    allok = True
    for src, outname, fam_en, fam_ja, ps in JOBS:
        if not os.path.exists(src):
            print(f"!! 源文件缺失: {src}")
            allok = False
            continue
        out = retarget(src, outname, fam_en, fam_ja, ps)
        print(f"[+] {outname}  ({os.path.getsize(out)} bytes)  <- {os.path.basename(src)}")
        if not verify(out, fam_en, fam_ja):
            allok = False
    print()
    if allok:
        print(">>> 全部生成并校验通过:", OUT)
    else:
        print(">>> 有问题, 见上方 !! 标记")
        sys.exit(1)


if __name__ == "__main__":
    main()
