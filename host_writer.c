/*
 * host_writer.c - Host program to write to shared memory
 * 
 * This program writes data to /dev/shm/ivshmem and measures performance
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define SHMEM_PATH "/dev/shm/ivshmem"
#define SHMEM_SIZE (64 * 1024 * 1024)  // 64MB
#define FRAME_SIZE (3840 * 2160 * 4)    // 4K RGBA frame (33MB)
#define MAGIC 0xDEADBEEF

// Shared memory layout
struct shared_data {
    uint32_t magic;           // Magic number to verify sync
    uint32_t sequence;        // Sequence number
    uint32_t guest_ack;       // Guest acknowledgment flag (0 = not read, 1 = read)
    uint32_t data_size;       // Size of data in buffer
    uint64_t padding;         // Padding for alignment
    uint8_t  buffer[0];       // Actual data buffer
};

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void test_latency(volatile struct shared_data *shm, int iterations)
{
    printf("\n=== Latency Test (Round-Trip) ===\n");
    printf("Sending %d messages and measuring round-trip time...\n", iterations);
    
    // Allocate array to store all latency measurements
    uint64_t *latencies = malloc(iterations * sizeof(uint64_t));
    if (!latencies) {
        perror("Failed to allocate latency array");
        return;
    }
    
    uint64_t total_latency = 0;
    int successful = 0;
    uint64_t min_latency = UINT64_MAX;
    uint64_t max_latency = 0;
    
    for (int i = 0; i < iterations; i++) {
        // Reset guest acknowledgment
        shm->guest_ack = 0;
        __sync_synchronize();
        
        // Write new message
        shm->sequence = i;
        shm->data_size = 0;
        shm->magic = MAGIC;
        __sync_synchronize();
        
        // Start timer
        uint64_t start_time = get_time_ns();
        
        // Wait for guest to acknowledge (timeout after 10ms)
        uint64_t timeout_time = start_time + 10000000ULL; // 10ms
        while (shm->guest_ack == 0) {
            if (get_time_ns() > timeout_time) {
                printf("  [%d] Timeout waiting for guest\n", i);
                break;
            }
        }
        
        uint64_t end_time = get_time_ns();
        
        if (shm->guest_ack == 1) {
            uint64_t latency = end_time - start_time;
            latencies[successful] = latency;  // Store measurement
            total_latency += latency;
            successful++;
            
            if (latency < min_latency) min_latency = latency;
            if (latency > max_latency) max_latency = latency;
            
            if (i % 100 == 0) {
                printf("  [%d] Round-trip: %lu ns (%.2f µs)\n", 
                       i, latency, latency / 1000.0);
            }
        }
        
        // Small delay between messages
        usleep(1000);
    }
    
    if (successful > 0) {
        printf("\nResults:\n");
        printf("  Successful: %d/%d\n", successful, iterations);
        printf("  Average round-trip: %lu ns (%.2f µs)\n", 
               total_latency / successful, 
               (total_latency / successful) / 1000.0);
        printf("  Min round-trip: %lu ns (%.2f µs)\n", 
               min_latency, min_latency / 1000.0);
        printf("  Max round-trip: %lu ns (%.2f µs)\n", 
               max_latency, max_latency / 1000.0);
        printf("  Estimated one-way: ~%lu ns (%.2f µs)\n", 
               (total_latency / successful) / 2,
               ((total_latency / successful) / 2) / 1000.0);
        
        // Export to CSV
        FILE *csv = fopen("latency_results.csv", "w");
        if (csv) {
            fprintf(csv, "iteration,latency_ns,latency_us\n");
            for (int i = 0; i < successful; i++) {
                fprintf(csv, "%d,%lu,%.3f\n", i, latencies[i], latencies[i] / 1000.0);
            }
            fclose(csv);
            printf("\n  ✓ Latency data exported to latency_results.csv\n");
        } else {
            perror("Failed to create CSV file");
        }
    } else {
        printf("\nNo successful measurements. Is the guest program running?\n");
    }
    
    free(latencies);
}

void test_bandwidth(volatile struct shared_data *shm)
{
    printf("\n=== Bandwidth Test ===\n");
    
    // Calculate available buffer size
    size_t header_size = offsetof(struct shared_data, buffer);
    size_t max_data_size = SHMEM_SIZE - header_size;
    
    // Test with a 4K frame (or max available)
    size_t test_size = (FRAME_SIZE < max_data_size) ? FRAME_SIZE : max_data_size;
    
    printf("Testing with %zu bytes (%.2f MB)...\n", 
           test_size, test_size / (1024.0 * 1024.0));
    
    // Generate test data (simple pattern)
    uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
    for (size_t i = 0; i < test_size; i++) {
        data_ptr[i] = i & 0xFF;
    }
    
    // Prepare header
    shm->guest_ack = 0;
    shm->sequence = 0xFFFF;  // Special marker for bandwidth test
    shm->data_size = test_size;
    shm->magic = MAGIC;
    __sync_synchronize();
    
    // Start timer
    uint64_t start_time = get_time_ns();
    
    // Wait for guest to confirm read (timeout after 1 second)
    uint64_t timeout = start_time + 1000000000ULL; // 1 second
    while (shm->guest_ack == 0) {
        if (get_time_ns() > timeout) {
            printf("Timeout waiting for guest to read data\n");
            return;
        }
    }
    
    uint64_t end_time = get_time_ns();
    uint64_t duration_ns = end_time - start_time;
    
    double duration_s = duration_ns / 1e9;
    double bandwidth_mbps = (test_size / (1024.0 * 1024.0)) / duration_s;
    double bandwidth_gbps = bandwidth_mbps / 1024.0;
    
    printf("\nResults:\n");
    printf("  Transfer time: %lu ns (%.2f ms)\n", 
           duration_ns, duration_ns / 1000000.0);
    printf("  Bandwidth: %.2f MB/s (%.2f GB/s)\n", 
           bandwidth_mbps, bandwidth_gbps);
    printf("  Theoretical max (DDR4-3200): ~25 GB/s\n");
    
    // Export to CSV
    FILE *csv = fopen("bandwidth_results.csv", "w");
    if (csv) {
        fprintf(csv, "test_size_bytes,test_size_mb,duration_ns,duration_ms,bandwidth_mbps,bandwidth_gbps\n");
        fprintf(csv, "%zu,%.2f,%lu,%.2f,%.2f,%.2f\n", 
                test_size, test_size / (1024.0 * 1024.0),
                duration_ns, duration_ns / 1000000.0,
                bandwidth_mbps, bandwidth_gbps);
        fclose(csv);
        printf("\n  ✓ Bandwidth data exported to bandwidth_results.csv\n");
    } else {
        perror("Failed to create bandwidth CSV file");
    }
}

int main(int argc, char *argv[])
{
    printf("Host Writer - ivshmem Performance Test\n");
    printf("=======================================\n\n");
    
    // Open shared memory
    int fd = open(SHMEM_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open shared memory");
        printf("Make sure the VM setup script has been run.\n");
        return 1;
    }
    
    // Get file size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    
    printf("Shared memory: %s (%ld bytes)\n", SHMEM_PATH, st.st_size);
    
    // Map shared memory
    void *ptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, 
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    volatile struct shared_data *shm = (volatile struct shared_data *)ptr;
    
    printf("Mapped at address: %p\n", ptr);
    printf("Data buffer size: %zu bytes\n", 
           st.st_size - offsetof(struct shared_data, buffer));
    
    // Wait for user
    printf("\nMake sure the guest program is running!\n");
    printf("Press Enter to start tests...\n");
    getchar();
    
    // Run tests
    test_latency(shm, 1000);
    
    printf("\nPress Enter to run bandwidth test...\n");
    getchar();
    
    test_bandwidth(shm);
    
    // Cleanup
    munmap(ptr, st.st_size);
    close(fd);
    
    printf("\nTests completed.\n");
    return 0;
}

