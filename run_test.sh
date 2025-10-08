#!/bin/bash

# Script to run the ivshmem performance test with comprehensive sanity checks

set -e  # Exit on any error

echo "=== IVSHMEM Performance Test ==="
echo ""

# Configuration
IVSHMEM_SIZE=64
SHMEM_PATH="/dev/shm/ivshmem"
SHMEM_FALLBACK="./ivshmem-shmem"
SSH_KEY="temp_id_rsa"
SSH_OPTS="-i $SSH_KEY -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
VM_USER="debian@localhost"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

error() {
    echo -e "${RED}ERROR: $1${NC}" >&2
    exit 1
}

warning() {
    echo -e "${YELLOW}WARNING: $1${NC}" >&2
}

success() {
    echo -e "${GREEN}✓ $1${NC}"
}

info() {
    echo "ℹ  $1"
}

# Sanity Check 1: VM is running
echo "Checking VM status..."
if ! pgrep -f qemu-system-x86_64 > /dev/null; then
    error "VM is not running! Start it first with: ./setup.sh"
fi
success "VM is running"

# Sanity Check 2: ivshmem support
echo "Checking ivshmem support..."
if ! pgrep -f "qemu-system-x86_64.*ivshmem" > /dev/null; then
    warning "VM may not have ivshmem support enabled"
    warning "Performance tests may not work correctly"
else
    success "VM has ivshmem support"
fi

# Sanity Check 3: SSH connectivity
echo "Checking SSH connectivity..."
if ! ssh $SSH_OPTS $VM_USER 'echo "SSH OK"' > /dev/null 2>&1; then
    error "Cannot connect to VM via SSH. Is the VM fully booted?"
fi
success "SSH connectivity OK"

# Sanity Check 4: VM has required tools
echo "Checking VM build environment..."
if ! ssh $SSH_OPTS $VM_USER 'command -v gcc > /dev/null'; then
    error "gcc not found in VM. The VM setup may have failed."
fi
if ! ssh $SSH_OPTS $VM_USER 'command -v make > /dev/null'; then
    error "make not found in VM. The VM setup may have failed."
fi
success "VM build environment OK"

# Sanity Check 5: ivshmem device in VM
echo "Checking ivshmem device in VM..."
if ! ssh $SSH_OPTS $VM_USER 'test -e /sys/bus/pci/devices/0000:00:03.0/resource2'; then
    error "ivshmem PCI device not found in VM. VM may not have ivshmem support."
fi
success "ivshmem PCI device found in VM"

# Sanity Check 6: Host build environment
echo "Checking host build environment..."
if ! command -v gcc > /dev/null; then
    error "gcc not found on host. Install with: apt-get install build-essential"
fi
if ! command -v make > /dev/null; then
    error "make not found on host. Install with: apt-get install build-essential"
fi
success "Host build environment OK"

# Sanity Check 7: OpenSSL development libraries
echo "Checking OpenSSL libraries..."
if ! pkg-config --exists openssl 2>/dev/null && ! test -f /usr/include/openssl/sha.h; then
    error "OpenSSL development libraries not found. Install with: apt-get install libssl-dev"
fi
success "OpenSSL libraries OK"

# Verify shared memory setup
echo "Verifying shared memory setup..."
EXPECTED_SIZE=$((IVSHMEM_SIZE * 1024 * 1024))

# Check if shared memory file exists
if [ ! -f "$SHMEM_PATH" ]; then
    error "Shared memory file not found: $SHMEM_PATH
Please run ./setup.sh first to create the VM and shared memory."
fi

# Check if shared memory file has correct size
CURRENT_SIZE=$(stat -c%s "$SHMEM_PATH" 2>/dev/null || echo "0")
if [ "$CURRENT_SIZE" -ne "$EXPECTED_SIZE" ]; then
    error "Shared memory file has wrong size: $CURRENT_SIZE bytes (expected: $EXPECTED_SIZE bytes)
Please run ./setup.sh to recreate the shared memory with correct size."
fi

success "Shared memory file verified: $SHMEM_PATH (${IVSHMEM_SIZE}MB)"

# Clean up any existing CSV files for fresh results
echo "Cleaning up previous test results..."
rm -f latency_results.csv bandwidth_results.csv
success "Previous results cleaned"

# Compile programs
echo "Compiling programs..."
if ! make all > /dev/null 2>&1; then
    error "Failed to compile host programs. Check compiler errors with: make all"
fi
success "Host programs compiled"

# Deploy and compile guest program
echo "Deploying guest program to VM..."
if ! make deploy > /dev/null 2>&1; then
    error "Failed to deploy guest program to VM. Check with: make deploy"
fi
success "Guest program deployed and compiled"

# Clean up any existing guest processes
echo "Cleaning up existing guest processes..."
ssh $SSH_OPTS $VM_USER 'sudo pkill -f guest_reader || true' > /dev/null 2>&1 || true
success "Cleanup complete"

# Note: No manual shared memory reset needed!
# The host_writer.c handles proper initialization with the state machine,
# and guest_reader.c is designed to be resilient to startup conditions
info "Shared memory initialization will be handled by host_writer.c"

# Run host writer
echo ""
info "Starting performance tests..."
echo ""

# Run latency test first
echo ""
info "=== RUNNING LATENCY TEST ==="
echo ""

# Start guest reader for latency test only
echo "Starting guest reader for latency test (100 iterations)..."
ssh $SSH_OPTS $VM_USER 'sudo /tmp/guest_reader -l 100' > /tmp/guest_latency.log 2>&1 &
LATENCY_GUEST_PID=$!

# Wait for guest to initialize
sleep 3

if ! kill -0 $LATENCY_GUEST_PID 2>/dev/null; then
    error "Latency guest failed to start. Check log: cat /tmp/guest_latency.log"
fi

# Run latency test on host (separate invocation)
if sudo ./host_writer -l 100; then
    success "Latency test completed successfully"
else
    warning "Latency test completed with issues"
fi

# Cleanup latency guest (handle gracefully even if process already exited)
echo "Cleaning up latency guest process..."
if kill -0 $LATENCY_GUEST_PID 2>/dev/null; then
    kill $LATENCY_GUEST_PID 2>/dev/null || true
    echo "  ✓ Latency guest process terminated"
else
    echo "  ✓ Latency guest process already completed"
fi
ssh $SSH_OPTS $VM_USER 'sudo pkill -f guest_reader || true' > /dev/null 2>&1
sleep 2

# Run bandwidth test separately  
echo ""
info "=== RUNNING BANDWIDTH TEST ==="
echo ""

# Start guest reader for bandwidth test only (30 total: 10 iterations × 3 frame types)
echo "Starting guest reader for bandwidth test (30 messages: 10×3 frame types)..."
ssh $SSH_OPTS $VM_USER 'sudo /tmp/guest_reader -c 30' > /tmp/guest_bandwidth.log 2>&1 &
BANDWIDTH_GUEST_PID=$!

# Wait for guest to initialize
sleep 3

if ! kill -0 $BANDWIDTH_GUEST_PID 2>/dev/null; then
    error "Bandwidth guest failed to start. Check log: cat /tmp/guest_bandwidth.log"
fi

# Run bandwidth test on host (separate invocation)
if sudo ./host_writer -b 10; then
    success "Bandwidth test completed successfully"
else
    warning "Bandwidth test completed with issues"
fi

# Cleanup bandwidth guest (handle gracefully)
echo "Cleaning up bandwidth guest process..."
if kill -0 $BANDWIDTH_GUEST_PID 2>/dev/null; then
    kill $BANDWIDTH_GUEST_PID 2>/dev/null || true
    echo "  ✓ Bandwidth guest process terminated"
else
    echo "  ✓ Bandwidth guest process already completed"
fi

# Final cleanup
ssh $SSH_OPTS $VM_USER 'sudo pkill -f guest_reader || true' > /dev/null 2>&1

# Show results
echo ""
echo "=========================================="
echo "Test Results:"
echo "=========================================="
if [ -f latency_results.csv ]; then
    LATENCY_COUNT=$(tail -n +2 latency_results.csv | wc -l)
    success "Latency results: $LATENCY_COUNT measurements in latency_results.csv"
else
    warning "No latency results found"
fi

if [ -f bandwidth_results.csv ]; then
    BANDWIDTH_COUNT=$(tail -n +2 bandwidth_results.csv | wc -l)
    success "Bandwidth results: $BANDWIDTH_COUNT measurements in bandwidth_results.csv"
else
    warning "No bandwidth results found"
fi

echo ""
info "To analyze results, run: python3 analyze_results.py"
info "Or with uv: uv run analyze_results.py"

# Automatically run analysis if results exist
if [ -f latency_results.csv ] || [ -f bandwidth_results.csv ]; then
    echo ""
    info "Running automatic analysis..."
    if command -v uv &> /dev/null; then
        if uv run analyze_results.py; then
            success "Analysis completed! Check generated plots and performance_report.txt"
        else
            warning "Analysis failed - you can run it manually with: uv run analyze_results.py"
        fi
    else
        warning "uv not found. Install with: curl -LsSf https://astral.sh/uv/install.sh | sh"
        info "Then run: uv run analyze_results.py"
    fi
fi
echo ""

success "Test completed!"