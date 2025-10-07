#!/bin/sh

# This is a VM-based solution with shared memory to measure the host -> guest communication 
# latency, bandwidth and pressure under load. The most straightforward approach is using 
# QEMU/KVM with ivshmem (Inter-VM Shared Memory device), which provides a PCI device that 
# maps to shared memory accessible by both host and guest.

# Needs to work on both ARM64 and x86_64 architecture. Linux only, as ivshmem is not available on macOS.

# Variables
IVSHMEM_SIZE=128
VM_NAME="ivshmem-vm"
VM_DISK="ivshmem-disk.qcow2"
CLOUD_IMAGE="debian-12-generic-amd64.qcow2"
CLOUD_IMAGE_URL="https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-generic-amd64.qcow2"
CERTIFICATE_FILE="temp_id_rsa"
SHMEM_FILE="./ivshmem-shmem"
QEMU_PATH="/usr/bin/qemu-system-x86_64"

# Detect OS
OS=$(uname -s)

# Create a IVSHMEM_SIZE shared memory file
if [ "$OS" = "Darwin" ]; then
  # macOS: use local file
  dd if=/dev/zero of=$SHMEM_FILE bs=1M count=$IVSHMEM_SIZE
  ACCEL_FLAG="-accel hvf"
  CPU_FLAG="-cpu host"
  SHMEM_PATH=$SHMEM_FILE
else
  # Linux: use /dev/shm if available, otherwise local file
  if [ -d /dev/shm ]; then
    dd if=/dev/zero of=/dev/shm/ivshmem bs=1M count=$IVSHMEM_SIZE
    SHMEM_PATH=/dev/shm/ivshmem
  else
    dd if=/dev/zero of=$SHMEM_FILE bs=1M count=$IVSHMEM_SIZE
    SHMEM_PATH=$SHMEM_FILE
  fi
  
  # Check if KVM is accessible
  if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL_FLAG="-enable-kvm"
    CPU_FLAG="-cpu host"
    echo "KVM acceleration is available"
  else
    ACCEL_FLAG="-accel tcg"
    CPU_FLAG="-cpu qemu64"
    echo "WARNING: KVM not accessible, using TCG (software emulation - slower)"
    echo "To enable KVM acceleration, add your user to the kvm group:"
    echo "  sudo usermod -aG kvm \$USER"
    echo "  Then log out and log back in"
    echo ""
  fi
fi

# Make sure we have the qemu-system-x86_64 command
which $QEMU_PATH > /dev/null
if [ $? -ne 0 ]; then
  echo "qemu-system-x86_64 not found"
  echo "Install with: brew install qemu (macOS) or apt-get install qemu-system-x86 (Linux)"
  exit 1
fi

# Check for build tools (needed to compile host/guest programs)
if ! command -v gcc > /dev/null; then
  echo "WARNING: gcc not found on host"
  echo "The host/guest test programs require gcc to compile."
  echo "Install with: apt-get install build-essential (Linux) or xcode-select --install (macOS)"
  echo ""
fi

# Download Debian cloud image if not present
if [ ! -f $CLOUD_IMAGE ]; then
  echo "Downloading Debian cloud image..."
  echo "This may take a few minutes..."
  if command -v wget > /dev/null; then
    wget -O $CLOUD_IMAGE $CLOUD_IMAGE_URL
  elif command -v curl > /dev/null; then
    curl -L -o $CLOUD_IMAGE $CLOUD_IMAGE_URL
  else
    echo "ERROR: Neither wget nor curl found. Please install one of them."
    exit 1
  fi
  
  if [ ! -f $CLOUD_IMAGE ]; then
    echo "ERROR: Failed to download cloud image"
    exit 1
  fi
fi

# Create VM disk from cloud image if it doesn't exist
if [ ! -f $VM_DISK ]; then
  echo "Creating VM disk from cloud image..."
  qemu-img create -f qcow2 -F qcow2 -b $CLOUD_IMAGE $VM_DISK 20G
fi

# Generate SSH key if it doesn't exist
if [ ! -f $CERTIFICATE_FILE ]; then
  echo "Generating SSH key..."
  ssh-keygen -t rsa -b 4096 -f $CERTIFICATE_FILE -N ""
fi

# Create cloud-init configuration
CLOUD_INIT_DIR="cloud-init-config"
mkdir -p $CLOUD_INIT_DIR

# Create meta-data
cat > $CLOUD_INIT_DIR/meta-data << EOF
instance-id: ivshmem-vm-001
local-hostname: ivshmem-vm
EOF

# Create user-data with SSH key
cat > $CLOUD_INIT_DIR/user-data << EOF
#cloud-config
users:
  - name: debian
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $(cat ${CERTIFICATE_FILE}.pub)

package_update: true
package_upgrade: true
packages:
  - build-essential
  - gcc
  - make

runcmd:
  - echo "VM setup complete" > /tmp/setup-complete
EOF

# Create cloud-init ISO
CLOUD_INIT_ISO="cloud-init.iso"
if command -v genisoimage > /dev/null; then
  genisoimage -output $CLOUD_INIT_ISO -volid cidata -joliet -rock $CLOUD_INIT_DIR/user-data $CLOUD_INIT_DIR/meta-data
elif command -v mkisofs > /dev/null; then
  mkisofs -output $CLOUD_INIT_ISO -volid cidata -joliet -rock $CLOUD_INIT_DIR/user-data $CLOUD_INIT_DIR/meta-data
else
  echo "ERROR: genisoimage or mkisofs not found. Install with: apt-get install genisoimage"
  exit 1
fi

# Check if ivshmem is available
HAS_IVSHMEM=0
if $QEMU_PATH -device help 2>&1 | grep -q ivshmem; then
  HAS_IVSHMEM=1
  echo "ivshmem device is available"
else
  echo "WARNING: ivshmem device is NOT available in this QEMU build"
  echo "The VM will start without shared memory support."
  echo ""
  echo "To get ivshmem support:"
  if [ "$OS" = "Darwin" ]; then
    echo "  - Use Linux instead (ivshmem is typically not in macOS QEMU builds)"
    echo "  - Or compile QEMU from source with --enable-ivshmem"
  else
    echo "  - Install qemu-system with ivshmem: apt-get install qemu-system-x86"
    echo "  - Or compile QEMU from source with --enable-ivshmem"
  fi
  echo ""
fi

# Launch QEMU VM with or without ivshmem Device
if [ $HAS_IVSHMEM -eq 1 ]; then
  # With ivshmem support
  $QEMU_PATH \
    -machine q35 \
    $CPU_FLAG \
    $ACCEL_FLAG \
    -m 2048 \
    -drive file=$VM_DISK,format=qcow2,if=virtio \
    -drive file=$CLOUD_INIT_ISO,format=raw,if=virtio \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -object memory-backend-file,id=hostmem,mem-path=$SHMEM_PATH,size=${IVSHMEM_SIZE}M,share=on \
    -device ivshmem-plain,memdev=hostmem \
    -nographic &
else
  # Without ivshmem (fallback)
  $QEMU_PATH \
    -machine q35 \
    $CPU_FLAG \
    $ACCEL_FLAG \
    -m 2048 \
    -drive file=$VM_DISK,format=qcow2,if=virtio \
    -drive file=$CLOUD_INIT_ISO,format=raw,if=virtio \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -nographic &
fi

QEMU_PID=$!
echo "QEMU started with PID $QEMU_PID"
echo "Checking if VM started successfully..."

# Wait up to 10 seconds for QEMU to either fail or stabilize
for i in $(seq 1 10); do
  if ! kill -0 $QEMU_PID 2>/dev/null; then
    echo "ERROR: QEMU process died unexpectedly"
    exit 1
  fi
  sleep 1
done

echo "VM is running. Waiting for boot to complete..."
# Now wait for SSH to become available (up to 50 more seconds)
for i in $(seq 1 50); do
  if ! kill -0 $QEMU_PID 2>/dev/null; then
    echo "ERROR: QEMU process died during boot"
    exit 1
  fi
  # Check if SSH port is responding
  if nc -z localhost 2222 2>/dev/null; then
    echo "VM booted successfully! SSH is available on port 2222"
    break
  fi
  sleep 1
done

echo ""
echo "=========================================="
echo "VM is ready!"
echo "=========================================="
echo "To connect to the VM:"
echo "  ssh -i $CERTIFICATE_FILE -p 2222 debian@localhost"
echo ""
echo "The VM has build-essential and gcc pre-installed via cloud-init."
echo ""
echo "To stop the VM:"
echo "  kill $QEMU_PID"
echo ""

