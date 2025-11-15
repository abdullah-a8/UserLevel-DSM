#!/bin/bash
# Script to run DSM as worker node (typically on local machine)

set -e

# Default values
MANAGER_HOST=""
MANAGER_PORT=5000
NODE_ID=1
TEST_BINARY="./build/test_test_multinode"

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --manager)
            MANAGER_HOST="$2"
            shift 2
            ;;
        --port)
            MANAGER_PORT="$2"
            shift 2
            ;;
        --id)
            NODE_ID="$2"
            shift 2
            ;;
        --test)
            TEST_BINARY="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 --manager <IP> [OPTIONS]"
            echo ""
            echo "Required:"
            echo "  --manager IP  Manager node IP address (cloud VM public IP)"
            echo ""
            echo "Options:"
            echo "  --port P      Manager port (default: 5000)"
            echo "  --id N        This node's ID (default: 1)"
            echo "  --test PATH   Path to test binary (default: ./build/test_test_multinode)"
            echo "  -h, --help    Show this help message"
            echo ""
            echo "Example:"
            echo "  $0 --manager 54.123.45.67 --port 5000 --id 1"
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
echo -e "${BLUE}║   DSM Worker Node (Multi-Node Test)   ║${NC}"
echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo ""

# Validate required arguments
if [ -z "$MANAGER_HOST" ]; then
    echo -e "${RED}Error: Manager host not specified${NC}"
    echo "Usage: $0 --manager <IP> [--port <PORT>] [--id <ID>]"
    echo ""
    echo "Example:"
    echo "  $0 --manager 54.123.45.67"
    exit 1
fi

# Check if test binary exists
if [ ! -f "$TEST_BINARY" ]; then
    echo -e "${RED}Error: Test binary not found at $TEST_BINARY${NC}"
    echo "Please run 'make' to build the tests first"
    exit 1
fi

echo "Configuration:"
echo "  - Manager host: $MANAGER_HOST"
echo "  - Manager port: $MANAGER_PORT"
echo "  - Node ID: $NODE_ID"
echo "  - Test binary: $TEST_BINARY"
echo ""

# Test connectivity
echo "Testing connectivity to manager..."
if command -v nc &> /dev/null; then
    if nc -zw3 $MANAGER_HOST $MANAGER_PORT 2>/dev/null; then
        echo -e "${GREEN}✓ Port $MANAGER_PORT is reachable on $MANAGER_HOST${NC}"
    else
        echo -e "${RED}✗ Cannot reach port $MANAGER_PORT on $MANAGER_HOST${NC}"
        echo ""
        echo "Troubleshooting steps:"
        echo "  1. Verify manager is running on $MANAGER_HOST"
        echo "  2. Check firewall allows port $MANAGER_PORT"
        echo "  3. Verify $MANAGER_HOST is the correct public IP"
        echo ""
        read -p "Continue anyway? (y/N) " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
    fi
else
    echo "Note: 'nc' not found, skipping connectivity check"
fi

echo ""
echo "Connecting to manager..."
echo "Press Ctrl+C to stop"
echo ""
echo "═══════════════════════════════════════════════════════════"
echo ""

# Run the test
exec $TEST_BINARY --worker --manager-host $MANAGER_HOST --manager-port $MANAGER_PORT --node-id $NODE_ID
