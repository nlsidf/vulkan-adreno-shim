import sys
reg = sys.argv[1]
TGT = "wqy-microhei.ttc"
ADDS = [("Tahoma","TrueType"),("Tahoma Bold","TrueType")]
FONTS_KEYS = [
    r'[Software\\Microsoft\\Windows\\CurrentVersion\\Fonts]',
    r'[Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts]',
]
def parse(lines):
    sec,order,cur={},[],None
    for ln in lines:
        s=ln.rstrip("\r\n")
        if s.startswith("[") and "]" in s:
            cur=s[:s.index("]")+1]
            if cur not in sec: sec[cur]=[]; order.append(cur)
        elif cur is not None: sec[cur].append(ln)
        else: sec.setdefault("__pre__",[]).append(ln); 
        if "__pre__" not in order: order.append("__pre__")
    return sec,order
def dump(sec,order):
    out=[]
    for k in order:
        if k!="__pre__": out.append(k+"\n")
        out.extend(sec[k])
    return out
lines=open(reg,encoding="utf-8",errors="surrogateescape").readlines()
sec,order=parse(lines)
for key in FONTS_KEYS:
    if key not in sec: sec[key]=[]; order.append(key)
    body=sec[key]; kept=[]
    existing=set()
    for ln in body:
        m=re.search if False else __import__('re').search
        import re
        mm=re.match(r'"([^"]+)"=',ln.rstrip("\r\n"))
        if mm and mm.group(1) in {f"{d} ({suf})" for (d,suf) in ADDS}: continue
        kept.append(ln)
    for (d,suf) in ADDS:
        kept.append('"%s (%s)"="%s"\n'%(d,suf,TGT))
    sec[key]=kept
open(reg,"w",encoding="utf-8",errors="surrogateescape").writelines(dump(sec,order))
print("OK: Tahoma registered to",TGT,"in both Fonts keys")
