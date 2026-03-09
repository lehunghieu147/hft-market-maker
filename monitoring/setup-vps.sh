#!/bin/bash
# Setup Prometheus agent + bot systemd service on VPS (Japan)
# Usage: ./setup-vps.sh [local_ip_or_tunnel_endpoint]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROM_VERSION="2.51.0"
PROM_DIR="/opt/prometheus"
LOCAL_ENDPOINT="${1:-http://158.51.108.170:9091}"
BOT_DIR="/root/workspace/hft-market-maker"
BOT_BIN="$BOT_DIR/market_maker"
BOT_CONFIG="$BOT_DIR/config/config.json"

echo "=== Setting up Prometheus agent on VPS ==="

# Download Prometheus if not present
if [ ! -f "$PROM_DIR/prometheus" ]; then
    echo "Downloading Prometheus v${PROM_VERSION}..."
    cd /tmp
    wget -q "https://github.com/prometheus/prometheus/releases/download/v${PROM_VERSION}/prometheus-${PROM_VERSION}.linux-amd64.tar.gz"
    tar xzf "prometheus-${PROM_VERSION}.linux-amd64.tar.gz"
    sudo mkdir -p "$PROM_DIR"
    sudo cp "prometheus-${PROM_VERSION}.linux-amd64/prometheus" "$PROM_DIR/"
    sudo cp "prometheus-${PROM_VERSION}.linux-amd64/promtool" "$PROM_DIR/"
    rm -rf "prometheus-${PROM_VERSION}.linux-amd64"*
    echo "Prometheus installed at $PROM_DIR"
else
    echo "Prometheus already installed at $PROM_DIR"
fi

# Copy and configure prometheus config
sudo cp "$SCRIPT_DIR/prometheus-vps.yml" "$PROM_DIR/prometheus.yml"

# Replace placeholder with actual endpoint
sudo sed -i "s|http://158.51.108.170:9091/api/v1/write|${LOCAL_ENDPOINT}/api/v1/write|g" "$PROM_DIR/prometheus.yml"

echo "Config written to $PROM_DIR/prometheus.yml"
echo "Remote write endpoint: ${LOCAL_ENDPOINT}/api/v1/write"

# Create systemd service
sudo tee /etc/systemd/system/prometheus.service > /dev/null << 'EOF'
[Unit]
Description=Prometheus Agent
After=network.target

[Service]
Type=simple
ExecStart=/opt/prometheus/prometheus \
    --config.file=/opt/prometheus/prometheus.yml \
    --storage.tsdb.retention.time=2h \
    --web.listen-address=:9090
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable prometheus
sudo systemctl start prometheus

# --- Bot systemd service ---
# Sets LD_LIBRARY_PATH so shared libs (prometheus-cpp, grpc++) are found
sudo tee /etc/systemd/system/market-maker.service > /dev/null << EOF
[Unit]
Description=HFT Market Maker Bot
After=network.target

[Service]
Type=simple
WorkingDirectory=${BOT_DIR}
Environment=LD_LIBRARY_PATH=${BOT_DIR}
ExecStart=${BOT_BIN} ${BOT_CONFIG}
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable market-maker

echo ""
echo "=== VPS setup complete ==="
echo "Prometheus agent:  :9090 (2h retention, remote_write → ${LOCAL_ENDPOINT})"
echo "Bot service:       market-maker (auto-restart on failure)"
echo ""
echo "Start bot:  sudo systemctl start market-maker"
echo "Bot logs:   sudo journalctl -u market-maker -f"
echo "Verify:     curl http://localhost:9090/api/v1/targets"
