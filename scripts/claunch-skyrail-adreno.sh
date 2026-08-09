#!/data/data/com.termux/files/usr/bin/bash
# 启动脚本: DXVK-Sarek + Adreno 540 Vulkan (本 shim → Sky1 WSI layer) 跑 星空列车 (64-bit Unity IL2CPP)
# 目的: 让 64 位游戏经 vulkan_adreno_icd.so(本仓库的 ICD shim) → Adreno HAL → X11 渲染
#
# 与 32 位 9-nine 的区别:
#   - 9-nine 是 32 位 WOW64 guest, HAL 返回 >4GB 指针 wine 表示不了 -> 白屏;
#     shim 在 vkMapMemory 里把 dmabuf 重映射到 <4GB (VK_ICD_MAP_LOW 默认开)。
#   - skyrail 是 64 位 guest (LP64), 能直接表示 >4GB 指针, 那个 bug 不存在,
#     所以这里必须 VK_ICD_MAP_LOW=0: 关掉无意义的低位映射 hack, shim 仍做
#     HAL 兼容处理 (剥离 win32 平台扩展 / 保留 WSI 扩展 / 清理 vkCreateDevice 的
#     重复 pNext / 缓存内存类型表), 但 vkAllocateMemory/vkMapMemory 全部透传。
#
# 关键: BOX64_MMAP32 必须 =0。
#   =1 时 box64 把进程内所有匿名 mmap (含 Adreno HAL 内部) 压到低 4GB,
#   导致 HAL 的 vkCreateDevice 直接 -1 (OUT_OF_HOST_MEMORY)。=0 让 HAL 留在高位。
ulimit -s 8192
PROJ="$HOME/proton11"
MID="85536ceb47e8aa768973fe1c6a227604"
LOG="${VK_TEST_LOG:-$PROJ/.build/adreno-skyrail.log}"

kill -9 $(pgrep wineserver) 2>/dev/null; sleep 1

export FAKE_MACHINE_ID="$MID"
export LD_PRELOAD="$HOME/fake_machineid.so:$PROJ/.build/vulkan_gpu.so"
"$PROJ/start-audio.sh" > /dev/null 2>&1 &

DIR="$HOME/basement/loveai/sky-rail-and-white-travel"
cd "$DIR" || exit 1

echo ">>> 启动 星空列车 (Sarek + Adreno 540 Vulkan, 64-bit, shim MAP_LOW=0) ..." | tee "$LOG"
WINEPREFIX="$PROJ/p11prefix" \
DISPLAY=:1 \
WINEDEBUG=-all \
WINEESYNC=1 \
PULSE_SERVER=tcp:127.0.0.1:4713 \
WINEDLLOVERRIDES="winepulse.drv=b;winealsa.drv=" \
VK_GPU_LOG=1 \
VK_LOADER_DEBUG=error,warn \
VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json \
VK_ICD_MAP_LOW=0 \
DXVK_LOG_LEVEL=info \
BOX64_MMAP32=0 \
BOX64_DYNAREC_SAFEFLAGS=2 \
BOX64_DYNAREC_BIGBLOCK=3 \
BOX64_DYNAREC_CALLRET=2 \
BOX64_DYNAREC_FORWARD=1024 \
BOX64_DYNAREC_ALIGNED_ATOMICS=1 \
BOX64_DYNAREC_STRONGMEM=2 \
BOX64_DYNAREC_WEAKBARRIER=1 \
BOX64_DYNAREC_FASTNAN=1 \
BOX64_DYNAREC_FASTROUND=1 \
BOX64_DYNACACHE=1 \
BOX64_DYNACACHE_FOLDER="$(readlink -f "$PROJ/.cache/dynacache/skyrail-adreno")" \
BOX64_RCFILE="$PROJ/box64/system/box64.box64rc" \
BOX64_LD_LIBRARY_PATH="/data/data/com.termux/files/usr/lib:$PROJ/xaw64_wine/proton-11/lib/wine/x86_64-unix:$PROJ/xaw64_wine/x86_64-windows:$PROJ/proton-11/lib" \
LD_LIBRARY_PATH="/data/data/com.termux/files/usr/lib" \
"$PROJ/box64/build/box64" "$PROJ/xaw64_wine/proton-11/bin/wine" game.exe 2>&1 | tee -a "$LOG"
echo "EXIT: $?" | tee -a "$LOG"
