#!/bin/bash

# Script to run the ivshmem performance test

echo "=== IVSHMEM Performance Test ==="
echo ""

# Check if VM is running
if ! pgrep -f qemu-system-x86_64 > /dev/null; then
    echo "ERROR: VM is not running!"
    echo "Start the VM first with: ./setup.sh"
    exit 1
fi

# Check if programs are compiled
if [ ! -f host_writer ]; then
    echo "ERROR: host_writer not found"
    echo "Compile with: gcc -Wall -O2 -std=c11 -o host_writer host_writer.c -lrt"
    exit 1
fi

# Start guest reader in background
echo "Starting guest reader on VM..."
ssh -i temp_id_rsa -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    debian@localhost 'sudo /tmp/guest_reader' > /tmp/guest_output.log 2>&1 &

GUEST_PID=$!

echo "Waiting 3 seconds for guest to initialize..."
sleep 3

# Check if guest is still running
if ! kill -0 $GUEST_PID 2>/dev/null; then
    echo "ERROR: Guest reader failed to start. Check /tmp/guest_output.log"
    cat /tmp/guest_output.log
    exit 1
fi

echo "Guest reader started (PID: $GUEST_PID)"
echo ""

# Run host writer
echo "Starting host writer..."
echo ""
./host_writer

# Cleanup
echo ""
echo "Stopping guest reader..."
kill $GUEST_PID 2>/dev/null || true

echo "Test completed!"


