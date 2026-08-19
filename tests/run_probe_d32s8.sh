#!/data/data/com.termux/files/usr/bin/bash
# run_probe_d32s8.sh — 逐组合跑 D32S8 RAW 探针, 捕获每组合退出码
# 用法: VK_ICD_FILENAMES=<json 指向目标 shim> ./run_probe_d32s8.sh
# 退出码: 0=完成  139=驱动段错误(SIGSEGV)  -1=创建失败(exit 1)  其他=异常
set -u
cd "$(dirname "$0")"
VK_TEST_RAW=1 VK_ICD_FILENAMES="${VK_ICD_FILENAMES:?need VK_ICD_FILENAMES}" \
LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-/data/data/com.termux/files/usr/lib}"

N=10
echo "=== D32_SFLOAT_S8_UINT(130) RAW 全组合探针 ==="
for i in $(seq 0 $((N-1))); do
    out=$(VK_TEST_RAW=1 VK_ICD_FILENAMES="$VK_ICD_FILENAMES" timeout 20 ./probe_d32s8 "$i" 2>&1)
    code=$?
    echo "---- combo $i (exit=$code) ----"
    echo "$out" | grep -E "combo|CREATE|RENDER|DEPTH_RESULT"
    if [ "$code" -eq 139 ]; then
        echo "  -> 驱动段错误 (SIGSEGV)"
    elif [ "$code" -ne 0 ]; then
        echo "  -> 退出码 $code"
    fi
done
echo "=== 探针结束 ==="
