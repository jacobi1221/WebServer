#!/bin/bash
# ==============================================================
#  TinyWebServer 性能测试脚本
#  依赖: wrk (推荐) 或 ab (Apache Bench)
#  用法: ./bench/run_bench.sh [host] [port]
# ==============================================================

set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-8080}"
BASE_URL="http://${HOST}:${PORT}"
RESULT_DIR="bench/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

mkdir -p "$RESULT_DIR"
RESULT_FILE="${RESULT_DIR}/bench_${TIMESTAMP}.txt"

info "Benchmark target: ${BASE_URL}"
info "Results will be saved to: ${RESULT_FILE}"
echo ""

# ---- 检查服务器是否在运行 ----
if ! curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/index.html" | grep -q "200"; then
    error "Server is not responding at ${BASE_URL}"
    error "Start with: ./build/TinyWebServer -p ${PORT}"
    exit 1
fi
info "Server is alive ✓"

# ---- 测试 payload 准备 ----
# 确保有测试文件
RESOURCE_DIR="resources"
if [ ! -f "${RESOURCE_DIR}/index.html" ]; then
    mkdir -p "$RESOURCE_DIR"
    cat > "${RESOURCE_DIR}/index.html" << 'HTMLEOF'
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>TinyWebServer Benchmark</title>
    <style>
        body { font-family: sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; }
        h1 { color: #333; }
        p { color: #666; line-height: 1.6; }
    </style>
</head>
<body>
    <h1>TinyWebServer — It Works!</h1>
    <p>If you see this page, the server is running correctly.</p>
    <p>This is a benchmark test page. Size: ~500 bytes.</p>
    <hr>
    <p><em>Powered by C++14 + epoll + thread pool</em></p>
</body>
</html>
HTMLEOF
    info "Created test resource: ${RESOURCE_DIR}/index.html"
fi

{
    echo "============================================"
    echo " TinyWebServer Benchmark Report"
    echo " Date: $(date)"
    echo " Target: ${BASE_URL}"
    echo "============================================"
    echo ""

    # ---- Test 1: 吞吐量测试 (wrk) ----
    if command -v wrk &> /dev/null; then
        info "Running wrk benchmarks..."

        for CONN in 100 500 1000; do
            echo "--- wrk: ${CONN} connections, 4 threads, 30s ---"
            wrk -t4 -c${CONN} -d30s --latency "${BASE_URL}/index.html" 2>&1 | tee -a "$RESULT_FILE"
            echo ""
            sleep 2  # 冷却
        done

    # ---- Test 2: 备用 ab 测试 ----
    elif command -v ab &> /dev/null; then
        info "wrk not found, using Apache Bench (ab)..."

        for CONN in 100 500 1000; do
            echo "--- ab: ${CONN} connections, 10000 requests ---"
            ab -n 10000 -c ${CONN} "${BASE_URL}/index.html" 2>&1 | tee -a "$RESULT_FILE"
            echo ""
            sleep 2
        done

    else
        warn "Neither wrk nor ab found. Install one:"
        warn "  Ubuntu: sudo apt install wrk"
        warn "  macOS:  brew install wrk"
        echo ""
    fi

    # ---- Test 3: Keep-Alive 效果测试 ----
    if command -v wrk &> /dev/null; then
        info "Testing Keep-Alive impact..."
        echo ""
        echo "--- wrk: with Keep-Alive ---"
        wrk -t4 -c200 -d10s "${BASE_URL}/index.html" 2>&1 | tee -a "$RESULT_FILE"
        echo ""
        echo "--- wrk: without Keep-Alive ---"
        wrk -t4 -c200 -d10s -H "Connection: close" "${BASE_URL}/index.html" 2>&1 | tee -a "$RESULT_FILE"
        echo ""
    fi

    # ---- Test 4: 并发极限测试 ----
    if command -v wrk &> /dev/null; then
        info "Testing concurrency limit (short burst)..."
        echo ""
        for CONN in 2000 5000; do
            echo "--- wrk: ${CONN} connections, 4 threads, 10s ---"
            wrk -t4 -c${CONN} -d10s --latency "${BASE_URL}/index.html" 2>&1 | tee -a "$RESULT_FILE" || true
            echo ""
            sleep 3
        done
    fi

} 2>&1 | tee -a "$RESULT_FILE"

echo ""
info "Benchmark complete. Results saved to: ${RESULT_FILE}"
