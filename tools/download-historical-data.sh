#!/bin/bash
# Download Binance historical depth snapshot data for backtesting.
# Source: data.binance.vision
#
# Usage: ./tools/download-historical-data.sh [SYMBOL] [DATE] [OUTPUT_DIR]
# Example: ./tools/download-historical-data.sh BTCUSDT 2024-01-15 data/

set -euo pipefail

SYMBOL="${1:-BTCUSDT}"
DATE="${2:-$(date -d 'yesterday' +%Y-%m-%d 2>/dev/null || date -v-1d +%Y-%m-%d)}"
OUTPUT_DIR="${3:-data}"

mkdir -p "$OUTPUT_DIR"

BASE_URL="https://data.binance.vision/data/spot/daily/bookDepth"
FILENAME="${SYMBOL}_bookDepth_${DATE}.zip"
URL="${BASE_URL}/${SYMBOL}/${FILENAME}"

echo "Downloading: ${URL}"
curl -fSL -o "${OUTPUT_DIR}/${FILENAME}" "${URL}" || {
    echo "Failed to download depth data. Trying aggTrades instead..."

    AGG_URL="https://data.binance.vision/data/spot/daily/aggTrades/${SYMBOL}/${SYMBOL}-aggTrades-${DATE}.zip"
    curl -fSL -o "${OUTPUT_DIR}/${SYMBOL}-aggTrades-${DATE}.zip" "${AGG_URL}" || {
        echo "Failed. Check symbol/date or visit https://data.binance.vision"
        exit 1
    }
    echo "Downloaded aggTrades to ${OUTPUT_DIR}/"
    cd "${OUTPUT_DIR}" && unzip -o "${SYMBOL}-aggTrades-${DATE}.zip"
    exit 0
}

echo "Downloaded to ${OUTPUT_DIR}/${FILENAME}"
cd "${OUTPUT_DIR}" && unzip -o "${FILENAME}"
echo "Done. CSV files in ${OUTPUT_DIR}/"
