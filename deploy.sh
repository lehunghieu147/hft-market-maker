#!/bin/bash
# ============================================================================
# HFT Market Maker - Deployment Script
# ============================================================================
# This script helps deploy the bot on VPS with proper API key setup
# ============================================================================

set -e  # Exit on error

echo "🚀 HFT Market Maker - Deployment Script"
echo "========================================"
echo ""

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    echo "❌ Docker is not installed!"
    echo "Install Docker first: curl -fsSL https://get.docker.com | sh"
    exit 1
fi

# Detect Docker Compose version (v2 built-in or v1 standalone)
COMPOSE_CMD=""
if docker compose version &> /dev/null; then
    COMPOSE_CMD="docker compose"
    echo "✅ Docker Compose V2 detected (built-in)"
elif command -v docker-compose &> /dev/null; then
    COMPOSE_CMD="docker-compose"
    echo "✅ Docker Compose V1 detected (standalone)"
else
    echo "❌ Docker Compose is not available!"
    echo ""
    echo "Your Docker version should have built-in Compose V2."
    echo "Try running: docker compose version"
    echo ""
    echo "If that fails, install standalone version:"
    echo "  sudo curl -L \"https://github.com/docker/compose/releases/latest/download/docker-compose-\$(uname -s)-\$(uname -m)\" -o /usr/local/bin/docker-compose"
    echo "  sudo chmod +x /usr/local/bin/docker-compose"
    exit 1
fi

echo "✅ Docker installed"
echo ""

# Check if .env file exists
if [ ! -f .env ]; then
    echo "⚠️  .env file not found!"
    echo ""

    # Interactive setup
    read -p "Do you want to create .env file now? (y/n): " CREATE_ENV

    if [ "$CREATE_ENV" = "y" ] || [ "$CREATE_ENV" = "Y" ]; then
        echo ""
        echo "📝 Creating .env file..."

        # Copy from example
        cp .env.example .env

        # Prompt for API key
        read -p "Enter your Binance API Key: " API_KEY
        read -sp "Enter your Binance API Secret: " API_SECRET
        echo ""
        read -p "Enter trading symbol (default: BTCUSDT): " SYMBOL
        SYMBOL=${SYMBOL:-BTCUSDT}

        read -p "Enter order size (default: 0.001): " ORDER_SIZE
        ORDER_SIZE=${ORDER_SIZE:-0.001}

        read -p "Enter spread percentage (default: 0.02): " SPREAD
        SPREAD=${SPREAD:-0.02}

        # Write to .env
        cat > .env << EOF
# Binance API Credentials
BINANCE_API_KEY=$API_KEY
BINANCE_API_SECRET=$API_SECRET

# Trading Parameters
SYMBOL=$SYMBOL
ORDER_SIZE=$ORDER_SIZE
SPREAD_PERCENTAGE=$SPREAD
EOF

        echo ""
        echo "✅ .env file created successfully!"
        echo ""
    else
        echo ""
        echo "❌ Deployment cancelled. Please create .env file manually:"
        echo "   1. cp .env.example .env"
        echo "   2. Edit .env with your credentials"
        exit 1
    fi
else
    echo "✅ .env file found"
    echo ""
fi

# Verify .env has required keys
if ! grep -q "BINANCE_API_KEY=.*[a-zA-Z0-9]" .env; then
    echo "⚠️  Warning: BINANCE_API_KEY appears to be empty in .env"
    read -p "Continue anyway? (y/n): " CONTINUE
    if [ "$CONTINUE" != "y" ] && [ "$CONTINUE" != "Y" ]; then
        exit 1
    fi
fi

# Create logs directory if not exists
mkdir -p logs
echo "✅ Logs directory ready"
echo ""

# Ask which action to perform
echo "Select deployment action:"
echo "1) Build and start (fresh deployment)"
echo "2) Rebuild and restart (update code)"
echo "3) Start existing container"
echo "4) Stop container"
echo "5) View logs"
echo "6) Show status"
echo ""
read -p "Enter choice [1-6]: " ACTION

case $ACTION in
    1)
        echo ""
        echo "🔨 Building and starting container..."
        $COMPOSE_CMD up -d --build
        echo ""
        echo "✅ Container started successfully!"
        echo ""
        echo "📊 View logs with: $COMPOSE_CMD logs -f"
        echo "🛑 Stop with: $COMPOSE_CMD down"
        ;;
    2)
        echo ""
        echo "🔄 Rebuilding and restarting..."
        $COMPOSE_CMD down
        $COMPOSE_CMD up -d --build
        echo ""
        echo "✅ Container restarted successfully!"
        ;;
    3)
        echo ""
        echo "▶️  Starting container..."
        $COMPOSE_CMD up -d
        echo ""
        echo "✅ Container started!"
        ;;
    4)
        echo ""
        echo "🛑 Stopping container..."
        $COMPOSE_CMD down
        echo ""
        echo "✅ Container stopped!"
        ;;
    5)
        echo ""
        echo "📊 Showing logs (Ctrl+C to exit)..."
        $COMPOSE_CMD logs -f
        ;;
    6)
        echo ""
        echo "📊 Container Status:"
        $COMPOSE_CMD ps
        echo ""
        echo "💾 Resource Usage:"
        docker stats --no-stream hft-market-maker
        ;;
    *)
        echo "❌ Invalid choice!"
        exit 1
        ;;
esac

echo ""
echo "✨ Done!"
