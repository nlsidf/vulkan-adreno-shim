#!/data/data/com.termux/files/usr/bin/bash
# 构建 Rust 版 ICD shim 并安装到游戏启动脚本期望的位置。
# 产出: ~/proton11/.build/vulkan_adreno_icd.so (与 C 版同名, json 无需改动)
set -e
cd "$(dirname "$0")"

echo ">>> cargo clippy --release (零警告检查)"
RUSTFLAGS="-D warnings" cargo clippy --release -- -D warnings 2>&1 | tail -3 || { echo "clippy 有警告, 中止"; exit 1; }

echo ">>> cargo build --release (cdylib, -D warnings)"
RUSTFLAGS="-D warnings" cargo build --release

OUT="$HOME/proton11/.build/vulkan_adreno_icd.so"
cp -f target/release/libvulkan_adreno_icd.so "$OUT"
echo ">>> 已安装: $OUT"
ls -l "$OUT"
