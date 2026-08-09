#!/data/data/com.termux/files/usr/bin/bash
# 调试版: 用 cmd.exe 启动 magicha.exe, 捕获控制台输出 + OutputDebugString
ulimit -s 8192
PROJ="$HOME/proton11"
MID="85536ceb47e8aa768973fe1c6a227604"
LOG="${VK_TEST_LOG:-$PROJ/.build/magiccharming-dbg.log}"

kill -9 $(pgrep wineserver) 2>/dev/null; sleep 1

export FAKE_MACHINE_ID="$MID"
export LD_PRELOAD="$HOME/fake_machineid.so:$PROJ/.build/vulkan_gpu.so"
"$PROJ/start-audio.sh" > /dev/null 2>&1 &

DIR="$PROJ/p11prefix/drive_d/magiccharming/magiccharming"
cd "$DIR" || exit 1

echo ">>> [DBG] cmd /c magicha.exe  $(date)" | tee "$LOG"
WINEPREFIX="$PROJ/p11prefix" \
DISPLAY=:1 \
WINEDEBUG="${DBG_CH:-+debugstr,+seh,+loaddll}" \
WINEESYNC=1 \
PULSE_SERVER=tcp:127.0.0.1:4713 \
WINEDLLOVERRIDES="winepulse.drv=b;winealsa.drv=${D3D9_OVERRIDE:+;d3d9=b}" \
VK_GPU_LOG=0 \
VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json \
DXVK_LOG_LEVEL=warn \
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
BOX64_DYNACACHE_FOLDER="$(readlink -f "$PROJ/.cache/dynacache/magiccharming")" \
BOX64_RCFILE="$PROJ/box64/system/box64.box64rc" \
BOX64_LD_LIBRARY_PATH="/data/data/com.termux/files/usr/lib:$PROJ/xaw64_wine/proton-11/lib/wine/x86_64-unix:$PROJ/xaw64_wine/x86_64-windows:$PROJ/proton-11/lib" \
LD_LIBRARY_PATH="/data/data/com.termux/files/usr/lib" \
"$PROJ/box64/build/box64" "$PROJ/xaw64_wine/proton-11/bin/wine" ${LAUNCH:-cmd /c magicha.exe} 2>&1 | tee -a "$LOG"
echo "EXIT: $?" | tee -a "$LOG"
