# Docker Deployment Guide

## Quick Start

### 1. Setup Environment Variables

```bash
# Copy environment template
cp .env.example .env

# Edit .env with your credentials
nano .env  # or vim, code, etc.
```

**IMPORTANT:** Never commit `.env` file to git! It's already in `.gitignore`.

### 2. Build and Run

#### Option A: Using Docker Compose (Recommended)

```bash
# Build and start
docker-compose up -d

# View logs
docker-compose logs -f

# Stop
docker-compose down
```

#### Option B: Using Docker CLI

```bash
# Build image
docker build -t hft-market-maker:latest .

# Run container
docker run -d \
  --name hft-market-maker \
  --restart unless-stopped \
  --env-file .env \
  -v $(pwd)/logs:/app/logs \
  -v $(pwd)/config:/app/config:ro \
  hft-market-maker:latest

# View logs
docker logs -f hft-market-maker

# Stop container
docker stop hft-market-maker
docker rm hft-market-maker
```

## VPS Deployment

### Prerequisites

1. **VPS Requirements:**
   - OS: Ubuntu 22.04+ or Debian 11+
   - RAM: 1GB minimum, 2GB recommended
   - CPU: 2 cores minimum
   - Disk: 10GB minimum
   - Stable internet connection

2. **Install Docker:**

```bash
# Update system
sudo apt-get update
sudo apt-get upgrade -y

# Install Docker
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh get-docker.sh

# Install Docker Compose
sudo apt-get install -y docker-compose-plugin

# Add user to docker group (optional - avoid sudo)
sudo usermod -aG docker $USER
newgrp docker
```

### Deploy to VPS

```bash
# 1. Clone repository (or upload via scp/rsync)
git clone <your-repo-url>
cd hft-market-maker

# 2. Create .env file
cp .env.example .env
nano .env  # Fill in your credentials

# 3. Create logs directory
mkdir -p logs

# 4. Build and start
docker-compose up -d

# 5. Check status
docker-compose ps
docker-compose logs -f market-maker
```

## Configuration

### Using Different Config Files

```bash
# Option 1: Edit docker-compose.yml
# Uncomment and modify the 'command' section:
# command: ["config/config.testnet.json"]

# Option 2: Override via docker run
docker run -d \
  --name hft-market-maker \
  --env-file .env \
  -v $(pwd)/logs:/app/logs \
  -v $(pwd)/config:/app/config:ro \
  hft-market-maker:latest \
  config/config.testnet.json
```

### Environment Variables Priority

1. `.env` file (highest priority)
2. `config/config.json` values
3. Default values in code

Available environment variables:
- `BINANCE_API_KEY`
- `BINANCE_API_SECRET`
- `SYMBOL`
- `ORDER_SIZE`
- `SPREAD_PERCENTAGE`

## Monitoring

### View Logs

```bash
# Follow logs
docker-compose logs -f

# Last 100 lines
docker-compose logs --tail=100

# Logs from specific time
docker logs --since 30m hft-market-maker
```

### Check Container Status

```bash
# Container status
docker-compose ps

# Resource usage
docker stats hft-market-maker

# Health check
docker inspect hft-market-maker --format='{{.State.Health.Status}}'
```

### Access Container Shell (Debug)

```bash
# Execute shell
docker-compose exec market-maker /bin/bash

# Or using docker
docker exec -it hft-market-maker /bin/bash
```

## Maintenance

### Update Application

```bash
# Pull latest code
git pull

# Rebuild and restart
docker-compose down
docker-compose up -d --build

# Or without downtime (if multiple instances)
docker-compose up -d --build --no-deps market-maker
```

### Backup

```bash
# Backup logs
tar -czf logs-backup-$(date +%Y%m%d).tar.gz logs/

# Backup config
tar -czf config-backup-$(date +%Y%m%d).tar.gz config/
```

### Clean Up

```bash
# Remove stopped containers
docker-compose down

# Remove images
docker rmi hft-market-maker:latest

# Clean all unused Docker resources
docker system prune -a --volumes
```

## Troubleshooting

### Container Won't Start

```bash
# Check logs
docker-compose logs market-maker

# Check if port conflicts
netstat -tulpn | grep <port>

# Verify environment variables
docker-compose config
```

### High Memory/CPU Usage

```bash
# Check resource usage
docker stats hft-market-maker

# Adjust limits in docker-compose.yml:
# deploy:
#   resources:
#     limits:
#       cpus: '1.0'
#       memory: 512M
```

### Network Issues

```bash
# Test connectivity from container
docker-compose exec market-maker ping -c 3 api.binance.com
docker-compose exec market-maker curl -I https://api.binance.com

# Check DNS
docker-compose exec market-maker cat /etc/resolv.conf
```

### Permission Issues

```bash
# Fix logs directory permissions
sudo chown -R 1000:1000 logs/

# Or use your user
sudo chown -R $USER:$USER logs/
```

## Security Best Practices

1. **API Keys:**
   - Use IP whitelist on Binance API keys
   - Enable only required permissions (no withdrawal)
   - Rotate keys periodically

2. **Container Security:**
   - Run as non-root user (already configured)
   - Keep Docker and base images updated
   - Use secrets management for production

3. **VPS Security:**
   - Configure firewall (UFW):
     ```bash
     sudo ufw enable
     sudo ufw allow ssh
     # Only allow required ports
     ```
   - Setup fail2ban for SSH protection
   - Regular security updates:
     ```bash
     sudo apt-get update && sudo apt-get upgrade -y
     ```

4. **Monitoring:**
   - Setup alerts for container failures
   - Monitor logs for errors/kill switch
   - Track P&L and position limits

## Production Recommendations

1. **Use host network mode for minimal latency:**
   ```yaml
   # In docker-compose.yml
   network_mode: "host"
   ```
   **Warning:** This exposes container to all host network interfaces.

2. **Configure log rotation:**
   ```yaml
   logging:
     driver: "json-file"
     options:
       max-size: "50m"
       max-file: "5"
   ```

3. **Setup external monitoring:**
   - Use Prometheus + Grafana for metrics
   - Setup alerts (email/SMS) for kill switch events
   - Track uptime and performance metrics

4. **Backup strategy:**
   - Automated daily backups of logs
   - Store config in secure location
   - Test restore procedures

## Performance Tuning

### For Ultra-Low Latency

1. **Host network mode:**
   ```yaml
   network_mode: "host"
   ```

2. **Disable unnecessary services:**
   ```bash
   sudo systemctl disable bluetooth
   sudo systemctl disable cups
   ```

3. **CPU pinning:**
   ```yaml
   cpuset: "0,1"  # Pin to specific CPU cores
   ```

4. **Increase network buffer:**
   ```bash
   # On VPS host
   sudo sysctl -w net.core.rmem_max=16777216
   sudo sysctl -w net.core.wmem_max=16777216
   ```

## Support

For issues or questions:
- Check logs: `docker-compose logs -f`
- Review configuration: `docker-compose config`
- Check container health: `docker ps`
- Monitor resources: `docker stats`
