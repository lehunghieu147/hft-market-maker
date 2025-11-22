# Quick Start Guide

## Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install -y build-essential cmake libssl-dev libcurl4-openssl-dev
```

## Build

```bash
mkdir -p build
cd build
cmake ..
make -j4
```

Executable: `build/bin/market_maker`

## Run

```bash
# From build directory
./bin/market_maker ../config/config_doge.json

# From project root
./build/bin/market_maker config/config_doge.json
```

## Config Files

All config files are in `config/` directory:

- `config/config_sei.json` - SEI/USDT trading
- `config/config_doge.json` - DOGE/USDT trading

Edit config to set your API key/secret before running.

See [README.md](README.md) for full documentation.
