# Monitoring Setup: VPS → Local Prometheus + Grafana

## Architecture

```
[VPS Japan]                              [Local Machine (VN)]
  Bot :8888/metrics                        Prometheus receiver :9091
  Prometheus agent :9090 ──tunnel:9092──→  Grafana :3000
```

Data flow: Bot → Prometheus VPS (scrape) → SSH tunnel → Prometheus Local (store) → Grafana (display)

## Prerequisites

- VPS: bot binary at `/root/workspace/hft-market-maker/market_maker`
- VPS: shared libs (prometheus-cpp, grpc++) in same directory
- Local: SSH key access to VPS configured as `vps` in `~/.ssh/config`

## Setup

### 1. Local Machine

```bash
cd monitoring
sudo ./setup-local.sh
```

Installs:
- Prometheus receiver on `:9091` (30d retention, `--web.enable-remote-write-receiver`)
- Grafana on `:3000` (admin/admin) with pre-provisioned dashboard

### 2. VPS

```bash
# Copy files to VPS
scp -O monitoring/setup-vps.sh monitoring/prometheus-vps.yml vps:/root/workspace/hft-market-maker/monitoring/

# SSH into VPS
ssh vps
cd /root/workspace/hft-market-maker/monitoring
chmod +x setup-vps.sh
./setup-vps.sh
```

Installs:
- Prometheus agent on `:9090` (2h retention, remote_write to `localhost:9092`)
- Bot systemd service `market-maker` with `LD_LIBRARY_PATH`

### 3. VPS Prometheus Config

Edit `/opt/prometheus/prometheus.yml` — ensure remote_write points to tunnel port:

```yaml
remote_write:
  - url: 'http://localhost:9092/api/v1/write'
```

### 4. SSH Reverse Tunnel (from local)

```bash
# VPS:9092 → Local:9091
ssh -R 9092:localhost:9091 vps -N &
```

This forwards VPS port 9092 back to local Prometheus receiver on 9091.

## Daily Operations

### Start everything

```bash
# 1. Local: start tunnel
ssh -R 9092:localhost:9091 vps -N &

# 2. VPS: start bot
ssh vps "sudo systemctl start market-maker"
```

### Stop

```bash
# Stop bot
ssh vps "sudo systemctl stop market-maker"

# Kill tunnel
kill %1  # or: pkill -f "ssh -R 9092"
```

### Check status

```bash
# VPS: bot status
ssh vps "sudo systemctl status market-maker"

# VPS: bot logs
ssh vps "sudo journalctl -u market-maker -f"

# VPS: Prometheus targets
ssh vps "curl -s http://localhost:9090/api/v1/targets"

# Local: query metrics
curl -s "http://localhost:9091/api/v1/query?query=mid_price" | python3 -m json.tool

# Local: Grafana dashboard
xdg-open http://localhost:3000
```

## Systemd Services

| Service | Location | Port | Purpose |
|---------|----------|------|---------|
| `market-maker` | VPS | 8888 | Trading bot + metrics endpoint |
| `prometheus` | VPS | 9090 | Scrape bot, remote_write via tunnel |
| `prometheus` | Local | 9091 | Receive remote_write, store 30d |
| `grafana-server` | Local | 3000 | Dashboard UI |

## Troubleshooting

| Issue | Fix |
|-------|-----|
| Tunnel `failed for listen port` | Port already in use on VPS — use different port |
| `libprometheus-cpp-pull.so` not found | Set `LD_LIBRARY_PATH=/root/workspace/hft-market-maker` |
| Grafana "No data" | Check tunnel alive: `curl localhost:9092` on VPS |
| Prometheus target "down" | Bot not running or port 8888 not listening |
| Empty query results on local | Tunnel disconnected — restart `ssh -R` command |
