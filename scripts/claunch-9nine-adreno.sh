#!/data/data/com.termux/files/usr/bin/bash
# 测试脚本: DXVK-Sarek + Adreno 540 Vulkan (Sky1 WSI layer) 跑 9-nine
# 目的: 验证 d3d → dxgi → Sarek → [WSI layer] → Adreno HAL → X11 管线是否通
# 依赖:
#   - ~/proton11/.build/vulkan_adreno_icd.so + .json (ICD shim → Adreno HAL)
#   - ~/proton11/.build/vulkan_gpu.so (namespace 链接, 可选, shim 自带)
#   - WSI layer 已注册为隐式 layer: /data/data/com.termux/files/usr/share/vulkan/implicit_layer.d/
# BOX64_MMAP32:
#   0 = HAL 的 mmap 留在高位, vkCreateDevice 成功; shim 在 vkMapMemory 里导出
#       dmabuf 自行映射到 <4GB, 让 wine wow64 能表示 (32 位 guest)。
#   1 = 所有匿名 mmap 被 box64 塞进低 4GB, Adreno HAL 内部失败, vkCreateDevice -1。
#   这里默认 0, 可通过环境变量覆盖。
ulimit -s 8192
PROJ="$HOME/proton11"
MID="85536ceb47e8aa768973fe1c6a227604"
LOG="${VK_TEST_LOG:-$PROJ/.build/adreno-9nine.log}"

kill -9 $(pgrep wineserver) 2>/dev/null; sleep 1

export FAKE_MACHINE_ID="$MID"
export LD_PRELOAD="$HOME/fake_machineid.so:$PROJ/.build/vulkan_gpu.so"
"$PROJ/start-audio.sh" > /dev/null 2>&1 &

DIR="$PROJ/p11prefix/drive_c/9-nine-Episode 1"
cd "$DIR" || exit 1

echo ">>> 启动 9-nine (Sarek + Adreno 540 Vulkan) ..." | tee "$LOG"
WINEPREFIX="$PROJ/p11prefix" \
DISPLAY=:1 \
WINEDEBUG=-all \
WINEESYNC=1 \
PULSE_SERVER=tcp:127.0.0.1:4713 \
WINEDLLOVERRIDES="winepulse.drv=b;winealsa.drv=" \
VK_GPU_LOG=1 \
VK_LOADER_DEBUG=error,warn \
VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json \
DXVK_LOG_LEVEL=info \
BOX64_MMAP32="${BOX64_MMAP32:-0}" \
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
BOX64_DYNACACHE_FOLDER="$(readlink -f "$PROJ/.cache/dynacache/9nine-adreno")" \
BOX64_RCFILE="$PROJ/box64/system/box64.box64rc" \
BOX64_LD_LIBRARY_PATH="/data/data/com.termux/files/usr/lib:$PROJ/xaw64_wine/proton-11/lib/wine/x86_64-unix:$PROJ/xaw64_wine/x86_64-windows:$PROJ/proton-11/lib" \
LD_LIBRARY_PATH="/data/data/com.termux/files/usr/lib" \
"$PROJ/box64/build/box64" "$PROJ/xaw64_wine/proton-11/bin/wine" "启动游戏.exe" 2>&1 | tee -a "$LOG"
echo "EXIT: $?" | tee -a "$LOG"
