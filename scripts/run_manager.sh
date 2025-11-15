#!/bin/bash
# Script to run DSM as manager node (typically on cloud VM)

set -e

# Default values
NODES=2
PORT=5000
TEST_BINARY="./build/test_test_multinode"

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --nodes)
            NODES="$2"
            shift 2
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --test)
            TEST_BINARY="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --nodes N    Number of nodes to wait for (default: 2)"
            echo "  --port P     Port to listen on (default: 5000)"
            echo "  --test PATH  Path to test binary (default: ./build/test_test_multinode)"
            echo "  -h, --help   Show this help message"
            echo ""
            echo "Example:"
            echo "  $0 --nodes 2 --port 5000"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║   DSM Manager Node (Multi-Node Test)  ║${NC}"
echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo ""

# Check if test binary exists
if [ ! -f "$TEST_BINARY" ]; then
    echo "Error: Test binary not found at $TEST_BINARY"
    echo "Please run 'make' to build the tests first"
    exit 1
fi

# Get public IP (works on AWS EC2, tries multiple methods)
echo "Detecting network configuration..."
PUBLIC_IP=$(curl -s http://checkip.amazonaws.com 2>/dev/null || curl -s ifconfig.me 2>/dev/null || hostname -I | awk '{print $1}')

if [ -z "$PUBLIC_IP" ]; then
    echo "Warning: Could not detect public IP"
    echo "Workers should connect to this machine's IP address"
else
    echo -e "${GREEN}Public IP: $PUBLIC_IP${NC}"
fi

echo ""
echo "Configuration:"
echo "  - Nodes expected: $NODES"
echo "  - Listening port: $PORT"
echo "  - Test binary: $TEST_BINARY"
echo ""

if [ -n "$PUBLIC_IP" ]; then
    echo -e "${GREEN}Workers should connect with:${NC}"
    echo -e "  ${BLUE}./scripts/run_worker.sh --manager $PUBLIC_IP --port $PORT --id 1${NC}"
    echo ""
fi

echo "Starting manager node..."
echo "Press Ctrl+C to stop"
echo ""
echo "═══════════════════════════════════════════════════════════"
echo ""

# Run the test
exec $TEST_BINARY --manager --nodes $NODES --port $PORT
