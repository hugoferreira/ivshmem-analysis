/*
 * guest_reader.c - Guest program to read from ivshmem PCI device
 * 
 * This program reads data from the ivshmem PCI BAR2 and measures performance
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <openssl/sha.h>

#define PCI_RESOURCE_PATH "/sys/bus/pci/devices/0000:00:03.0/resource2"
#define MAGIC 0xDEADBEEF

// Guest acknowledgment states (must match host)
#define GUEST_ACK_NONE       0  // No acknowledgment
#define GUEST_ACK_RECEIVED   1  // Message received, processing started
#define GUEST_ACK_PROCESSED  2  // Message processed successfully
#define GUEST_ACK_ERROR      3  // Message processing failed

// Shared memory layout (must match host)
struct shared_data {
    uint32_t magic;
    uint32_t sequence;
    uint32_t guest_ack;       // Guest acknowledgment state (see defines above)
    uint32_t data_size;
    uint32_t signature_magic; // 0xDEADBEEF at start of signature
    uint8_t  data_sha256[32]; // SHA256 of the data buffer
    uint32_t error_code;      // Error code if guest_ack == GUEST_ACK_ERROR
    uint32_t padding;         // Padding for alignment
    uint8_t  buffer[0];
};

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void monitor_latency(volatile struct shared_data *shm)
{
    printf("Monitoring for messages from host...\n");
    printf("Waiting for host to send data...\n\n");
    fflush(stdout);
    
    printf("DEBUG: Entered monitor_latency function\n");
    fflush(stdout);
    
    uint32_t last_sequence = 0xFFFFFFFF;
    int message_count = 0;
    
    printf("DEBUG: Starting main monitoring loop\n");
    fflush(stdout);
    
    while (1) {
        // Wait for new message with valid magic
        if (shm->magic == MAGIC && shm->sequence != last_sequence) {
            printf("DEBUG: New message detected! Magic=0x%08X, Seq=%u, LastSeq=%u\n", 
                   shm->magic, shm->sequence, last_sequence);
            fflush(stdout);
            
            uint32_t seq = shm->sequence;
            uint32_t data_size = shm->data_size;
            
            // Acknowledge message received immediately
            shm->guest_ack = GUEST_ACK_RECEIVED;
            __sync_synchronize();
            
            printf("DEBUG: Message acknowledged as RECEIVED\n");
            fflush(stdout);
            
            last_sequence = seq;
            message_count++;
            
            // For latency tests, immediately mark as processed (no heavy computation)
            if (seq != 0xFFFF) {
                shm->guest_ack = GUEST_ACK_PROCESSED;
                __sync_synchronize();
                
                if (message_count % 100 == 0) {
                    printf("  [%u] Received and processed\n", seq);
                }
            }
            
            // Check for bandwidth test marker (0xFFFF and above)
            if (seq >= 0xFFFF) {
                printf("\n=== Bandwidth Test Message Received ===\n");
                printf("Data size: %u bytes (%.2f MB)\n", 
                       data_size, data_size / (1024.0 * 1024.0));
                fflush(stdout);
                
                printf("DEBUG: Checking signature magic...\n");
                fflush(stdout);
                
                // Verify signature magic
                if (shm->signature_magic != MAGIC) {
                    printf("ERROR: Invalid signature magic: 0x%08X (expected 0x%08X)\n", 
                           shm->signature_magic, MAGIC);
                    continue;
                }
                
                printf("DEBUG: Signature magic OK, starting buffer read...\n");
                fflush(stdout);
                
                // Force read of entire buffer to ensure it's loaded from memory
                printf("DEBUG: Starting cache-line read of %u bytes...\n", data_size);
                fflush(stdout);
                
                uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
                volatile uint8_t dummy = 0;
                for (size_t i = 0; i < data_size; i += 64) { // Every cache line
                    dummy ^= data_ptr[i];
                }
                (void)dummy; // Prevent optimization
                
                printf("DEBUG: Cache-line read completed, starting SHA256...\n");
                fflush(stdout);
                
                // Calculate SHA256 of received data
                uint8_t calculated_hash[32];
                SHA256_CTX ctx;
                SHA256_Init(&ctx);
                SHA256_Update(&ctx, data_ptr, data_size);
                SHA256_Final(calculated_hash, &ctx);
                
                printf("DEBUG: SHA256 calculation completed\n");
                fflush(stdout);
                
                // Compare with expected hash (copy to avoid volatile qualifier issues)
                uint8_t expected_hash[32];
                memcpy(expected_hash, (const void *)shm->data_sha256, 32);
                int hash_match = (memcmp(calculated_hash, expected_hash, 32) == 0);
                
                if (hash_match) {
                    printf("✓ Data integrity verified: SHA256 match (full buffer)\n");
                    printf("  Signature: 0xDEADBEEF + SHA256 verified\n");
                    
                    // Mark processing as completed successfully
                    shm->guest_ack = GUEST_ACK_PROCESSED;
                    __sync_synchronize();
                    
                    printf("DEBUG: Message acknowledged as PROCESSED (success)\n");
                    fflush(stdout);
                    
                    // Wait a moment for host to read the result, then reset to ready state
                    usleep(10000); // 10ms
                    shm->guest_ack = GUEST_ACK_NONE;
                    __sync_synchronize();
                    
                    printf("DEBUG: Reset to ready state for next message\n");
                    fflush(stdout);
                } else {
                    printf("ERROR: Data integrity check failed - SHA256 mismatch!\n");
                    printf("  Expected: ");
                    for (int i = 0; i < 32; i++) printf("%02x", expected_hash[i]);
                    printf("\n  Got:      ");
                    for (int i = 0; i < 32; i++) printf("%02x", calculated_hash[i]);
                    printf("\n");
                    
                    // Mark processing as failed
                    shm->error_code = 1; // Data integrity error
                    shm->guest_ack = GUEST_ACK_ERROR;
                    __sync_synchronize();
                    
                    printf("DEBUG: Message acknowledged as ERROR (integrity failure)\n");
                    fflush(stdout);
                    
                    // Wait a moment for host to read the error, then reset to ready state
                    usleep(10000); // 10ms
                    shm->guest_ack = GUEST_ACK_NONE;
                    __sync_synchronize();
                    
                    printf("DEBUG: Reset to ready state for next message\n");
                    fflush(stdout);
                }
            }
        }
        
        // Use interrupt instead of busy-waiting
        
        // Small sleep to avoid busy-waiting
        usleep(10);
    }
}

int main(int argc, char *argv[])
{
    printf("Guest Reader - ivshmem Performance Test\n");
    printf("========================================\n\n");
    fflush(stdout);
    
    printf("DEBUG: Starting main function\n");
    fflush(stdout);
    
    // Check if running in VM
    printf("DEBUG: Checking PCI device access...\n");
    fflush(stdout);
    
    if (access(PCI_RESOURCE_PATH, F_OK) != 0) {
        printf("ERROR: ivshmem device not found at %s\n", PCI_RESOURCE_PATH);
        printf("Make sure you're running this inside the VM.\n");
        printf("\nTrying to find ivshmem device...\n");
        system("lspci | grep -i 'shared memory'");
        return 1;
    }
    printf("DEBUG: PCI device found\n");
    fflush(stdout);
    
    // Open PCI resource
    printf("DEBUG: Opening PCI resource...\n");
    fflush(stdout);
    
    int fd = open(PCI_RESOURCE_PATH, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Failed to open PCI resource");
        printf("\nTry running with sudo:\n");
        printf("  sudo %s\n", argv[0]);
        return 1;
    }
    printf("DEBUG: PCI resource opened successfully\n");
    fflush(stdout);
    
    // Get resource size
    printf("DEBUG: Getting resource size...\n");
    fflush(stdout);
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    
    printf("PCI Resource: %s\n", PCI_RESOURCE_PATH);
    printf("Resource size: %ld bytes (%ld MB)\n", 
           st.st_size, st.st_size / (1024 * 1024));
    fflush(stdout);
    
    // Map PCI BAR2 (shared memory region)
    printf("DEBUG: Mapping memory...\n");
    fflush(stdout);
    
    void *ptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, 
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        printf("\nTry running with sudo:\n");
        printf("  sudo %s\n", argv[0]);
        close(fd);
        return 1;
    }
    printf("DEBUG: Memory mapped successfully\n");
    fflush(stdout);
    
    volatile struct shared_data *shm = (volatile struct shared_data *)ptr;
    
    printf("Mapped at address: %p\n", ptr);
    printf("Ready to receive data from host.\n\n");
    fflush(stdout);
    
    // Show current values
    printf("DEBUG: Reading initial values...\n");
    fflush(stdout);
    
    printf("Initial values:\n");
    printf("  Magic: 0x%08X\n", shm->magic);
    printf("  Sequence: %u\n", shm->sequence);
    printf("  Guest ACK: %u\n", shm->guest_ack);
    printf("  Data size: %u\n", shm->data_size);
    printf("\n");
    fflush(stdout);
    
    // Start monitoring
    printf("DEBUG: Starting monitor_latency function...\n");
    fflush(stdout);
    
    monitor_latency(shm);
    
    // Cleanup (unreachable in this version)
    munmap(ptr, st.st_size);
    close(fd);
    
    return 0;
}

