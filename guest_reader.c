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

#define PCI_RESOURCE_PATH "/sys/bus/pci/devices/0000:00:03.0/resource2"
#define MAGIC 0xDEADBEEF

// Shared memory layout (must match host)
struct shared_data {
    uint32_t magic;
    uint32_t sequence;
    uint32_t guest_ack;
    uint32_t data_size;
    uint64_t padding;
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
    
    uint32_t last_sequence = 0xFFFFFFFF;
    int message_count = 0;
    
    while (1) {
        // Wait for new message with valid magic
        if (shm->magic == MAGIC && shm->sequence != last_sequence) {
            uint32_t seq = shm->sequence;
            uint32_t data_size = shm->data_size;
            
            // Acknowledge immediately
            shm->guest_ack = 1;
            __sync_synchronize();
            
            last_sequence = seq;
            message_count++;
            
            if (message_count % 100 == 0) {
                printf("  [%u] Received and acknowledged\n", seq);
            }
            
            // Check for bandwidth test marker
            if (seq == 0xFFFF) {
                printf("\n=== Bandwidth Test Message Received ===\n");
                printf("Data size: %u bytes (%.2f MB)\n", 
                       data_size, data_size / (1024.0 * 1024.0));
                
                // Verify data integrity
                uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
                int errors = 0;
                size_t check_size = (data_size > 10000) ? 10000 : data_size;
                for (size_t i = 0; i < check_size; i++) {
                    if (data_ptr[i] != (i & 0xFF)) {
                        errors++;
                        if (errors <= 5) {
                            printf("  Error at byte %zu: expected 0x%02X, got 0x%02X\n",
                                   i, (unsigned)(i & 0xFF), data_ptr[i]);
                        }
                    }
                }
                
                if (errors > 0) {
                    printf("WARNING: Data integrity check failed (%d errors in first %zu bytes)\n", 
                           errors, check_size);
                } else {
                    printf("✓ Data integrity verified (first %zu bytes)\n", check_size);
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
    
    // Check if running in VM
    if (access(PCI_RESOURCE_PATH, F_OK) != 0) {
        printf("ERROR: ivshmem device not found at %s\n", PCI_RESOURCE_PATH);
        printf("Make sure you're running this inside the VM.\n");
        printf("\nTrying to find ivshmem device...\n");
        system("lspci | grep -i 'shared memory'");
        return 1;
    }
    
    // Open PCI resource
    int fd = open(PCI_RESOURCE_PATH, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Failed to open PCI resource");
        printf("\nTry running with sudo:\n");
        printf("  sudo %s\n", argv[0]);
        return 1;
    }
    
    // Get resource size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    
    printf("PCI Resource: %s\n", PCI_RESOURCE_PATH);
    printf("Resource size: %ld bytes (%ld MB)\n", 
           st.st_size, st.st_size / (1024 * 1024));
    
    // Map PCI BAR2 (shared memory region)
    void *ptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, 
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        printf("\nTry running with sudo:\n");
        printf("  sudo %s\n", argv[0]);
        close(fd);
        return 1;
    }
    
    volatile struct shared_data *shm = (volatile struct shared_data *)ptr;
    
    printf("Mapped at address: %p\n", ptr);
    printf("Ready to receive data from host.\n\n");
    
    // Show current values
    printf("Initial values:\n");
    printf("  Magic: 0x%08X\n", shm->magic);
    printf("  Sequence: %u\n", shm->sequence);
    printf("  Guest ACK: %u\n", shm->guest_ack);
    printf("  Data size: %u\n", shm->data_size);
    printf("\n");
    
    // Start monitoring
    monitor_latency(shm);
    
    // Cleanup (unreachable in this version)
    munmap(ptr, st.st_size);
    close(fd);
    
    return 0;
}

