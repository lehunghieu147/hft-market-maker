#!/bin/bash
# Setup Prometheus (receiver) + Grafana on local machine
# Usage: ./setup-local.sh

set -e

PROM_VERSION="2.51.0"
GRAFANA_VERSION="11.0.0"
PROM_DIR="/opt/prometheus"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Setting up Prometheus + Grafana on local machine ==="

# --- Prometheus ---
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

sudo cp "$SCRIPT_DIR/prometheus-local.yml" "$PROM_DIR/prometheus.yml"

# Systemd service with remote-write receiver enabled
sudo tee /etc/systemd/system/prometheus.service > /dev/null << 'EOF'
[Unit]
Description=Prometheus Receiver
After=network.target

[Service]
Type=simple
ExecStart=/opt/prometheus/prometheus \
    --config.file=/opt/prometheus/prometheus.yml \
    --web.enable-remote-write-receiver \
    --web.listen-address=:9091 \
    --storage.tsdb.retention.time=30d
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable prometheus
sudo systemctl start prometheus
echo "Prometheus receiver running on :9091 (30d retention)"

# --- Grafana ---
if ! command -v grafana-server &> /dev/null; then
    echo "Installing Grafana..."
    sudo apt-get install -y apt-transport-https software-properties-common
    sudo mkdir -p /etc/apt/keyrings/
    wget -q -O - https://apt.grafana.com/gpg.key | gpg --dearmor | sudo tee /etc/apt/keyrings/grafana.gpg > /dev/null
    echo "deb [signed-by=/etc/apt/keyrings/grafana.gpg] https://apt.grafana.com stable main" | sudo tee /etc/apt/sources.list.d/grafana.list
    sudo apt-get update
    sudo apt-get install -y grafana
else
    echo "Grafana already installed"
fi

# Copy provisioning configs
sudo mkdir -p /etc/grafana/provisioning/datasources
sudo mkdir -p /etc/grafana/provisioning/dashboards

# Datasource pointing to local Prometheus receiver
sudo tee /etc/grafana/provisioning/datasources/prometheus.yml > /dev/null << 'EOF'
apiVersion: 1
datasources:
  - name: Prometheus
    type: prometheus
    access: proxy
    url: http://localhost:9091
    isDefault: true
    editable: false
EOF

# Dashboard provisioning
sudo cp "$SCRIPT_DIR/grafana/provisioning/dashboards/dashboard.yml" /etc/grafana/provisioning/dashboards/
sudo cp "$SCRIPT_DIR/grafana/provisioning/dashboards/trading-metrics.json" /etc/grafana/provisioning/dashboards/

sudo systemctl daemon-reload
sudo systemctl enable grafana-server
sudo systemctl start grafana-server

echo ""
echo "=== Local monitoring setup complete ==="
echo "Prometheus receiver: http://localhost:9091 (accepting remote_write)"
echo "Grafana dashboard:   http://localhost:3000 (admin/admin)"
echo ""
echo "Next: Open http://localhost:3000 → Dashboard → HFT Market Maker"
