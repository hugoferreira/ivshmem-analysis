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
#include <openssl/sha.h>
#include <openssl/rand.h>

#define SHMEM_PATH "/dev/shm/ivshmem"
#define SHMEM_SIZE (64 * 1024 * 1024)  // 64MB
#define FRAME_SIZE (3840 * 2160 * 4)    // 4K RGBA frame (33MB)
#define MAGIC 0xDEADBEEF

// Guest acknowledgment states
#define GUEST_ACK_NONE       0  // No acknowledgment
#define GUEST_ACK_RECEIVED   1  // Message received, processing started
#define GUEST_ACK_PROCESSED  2  // Message processed successfully
#define GUEST_ACK_ERROR      3  // Message processing failed

// Shared memory layout
struct shared_data {
    uint32_t magic;           // Magic number to verify sync
    uint32_t sequence;        // Sequence number
    uint32_t guest_ack;       // Guest acknowledgment state (see defines above)
    uint32_t data_size;       // Size of data in buffer
    uint32_t signature_magic; // 0xDEADBEEF at start of signature
    uint8_t  data_sha256[32]; // SHA256 of the data buffer
    uint32_t error_code;      // Error code if guest_ack == GUEST_ACK_ERROR
    uint32_t padding;         // Padding for alignment
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
        shm->guest_ack = GUEST_ACK_NONE;
        shm->error_code = 0;
        __sync_synchronize();
        
        // Write new message
        shm->sequence = i;
        shm->data_size = 0;
        shm->magic = MAGIC;
        __sync_synchronize();
        
        // Start timer
        uint64_t start_time = get_time_ns();
        
        // Wait for guest to receive message (timeout after 5ms)
        uint64_t timeout_time = start_time + 5000000ULL; // 5ms
        while (shm->guest_ack == GUEST_ACK_NONE) {
            if (get_time_ns() > timeout_time) {
                printf("  [%d] Timeout waiting for guest to receive message\n", i);
                break;
            }
        }
        
        if (shm->guest_ack == GUEST_ACK_NONE) {
            continue; // Skip this iteration
        }
        
        // Wait for guest to complete processing (timeout after 5ms more)
        timeout_time = get_time_ns() + 5000000ULL; // 5ms
        while (shm->guest_ack == GUEST_ACK_RECEIVED) {
            if (get_time_ns() > timeout_time) {
                printf("  [%d] Timeout waiting for guest to process message\n", i);
                break;
            }
        }
        
        uint64_t end_time = get_time_ns();
        
        if (shm->guest_ack == GUEST_ACK_PROCESSED) {
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

// Cache flush function to ensure data comes from memory, not cache
static void flush_cache_range(void *addr, size_t len) {
    // Force cache flush by reading every cache line with volatile access
    volatile char *ptr = (volatile char *)addr;
    for (size_t i = 0; i < len; i += 64) { // 64-byte cache lines
        (void)ptr[i]; // Force read to flush cache
    }
    __sync_synchronize(); // Memory barrier
}

// Generate random frame buffer (width x height x 24bpp)
static void generate_random_frame(uint8_t *buffer, int width, int height) {
    size_t frame_size = width * height * 3; // 24bpp = 3 bytes per pixel
    
    // Generate cryptographically random data to avoid cache-friendly patterns
    if (RAND_bytes(buffer, frame_size) != 1) {
        // Fallback to pseudo-random if OpenSSL fails
        // Use high-resolution time + iteration counter for better seed diversity
        static int iteration_counter = 0;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        unsigned int seed = (unsigned int)(ts.tv_sec ^ ts.tv_nsec ^ (++iteration_counter));
        srand(seed);
        
        for (size_t i = 0; i < frame_size; i++) {
            buffer[i] = rand() & 0xFF;
        }
    }
}

// Calculate SHA256 hash of buffer
static void calculate_sha256(const uint8_t *data, size_t len, uint8_t *hash) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(hash, &ctx);
}

void test_bandwidth(volatile struct shared_data *shm, int iterations)
{
    printf("\n=== Bandwidth Test ===\n");
    printf("Testing with random frame data to avoid cache effects...\n");
    
    // Calculate available buffer size
    size_t header_size = offsetof(struct shared_data, buffer);
    size_t max_data_size = SHMEM_SIZE - header_size;
    
    // Test different frame sizes
    struct {
        int width, height, bpp;
        const char *name;
    } test_frames[] = {
        {1920, 1080, 3, "1080p"},  // 24bpp = 3 bytes per pixel
        {2560, 1440, 3, "1440p"},  // 24bpp = 3 bytes per pixel
        {3840, 2160, 3, "4K"},     // 24bpp = 3 bytes per pixel
        {0, 0, 0, NULL} // Sentinel
    };
    
    // Open CSV file for results
    FILE *csv = fopen("bandwidth_results.csv", "w");
    if (csv) {
        fprintf(csv, "iteration,frame_type,width,height,bpp,test_size_bytes,test_size_mb,duration_ns,duration_ms,bandwidth_mbps,bandwidth_gbps,data_verified\n");
    }
    
    for (int frame_idx = 0; test_frames[frame_idx].name != NULL; frame_idx++) {
        int width = test_frames[frame_idx].width;
        int height = test_frames[frame_idx].height;
        int bpp = test_frames[frame_idx].bpp;
        size_t frame_size = width * height * bpp;
        
        if (frame_size > max_data_size) {
            printf("Skipping %s (%dx%d): frame too large (%zu bytes > %zu bytes)\n", 
                   test_frames[frame_idx].name, width, height, frame_size, max_data_size);
            continue;
        }
        
        printf("\n--- Testing %s (%dx%d, %.2f MB) ---\n", 
               test_frames[frame_idx].name, width, height, frame_size / (1024.0 * 1024.0));
        
        double total_bandwidth = 0.0;
        int successful_tests = 0;
        
        for (int iter = 0; iter < iterations; iter++) {
            // Wait for guest to finish any previous processing
            if (iter > 0) {
                printf("  Waiting for guest to be ready...");
                fflush(stdout);
                
                // Wait up to 1 second for guest to be ready for next message
                uint64_t wait_start = get_time_ns();
                uint64_t wait_timeout = wait_start + 1000000000ULL; // 1 second
                while (shm->guest_ack != GUEST_ACK_NONE && get_time_ns() < wait_timeout) {
                    usleep(1000); // 1ms
                }
                
                if (shm->guest_ack != GUEST_ACK_NONE) {
                    printf(" TIMEOUT (guest not ready)\n");
                    if (csv) {
                        fprintf(csv, "%d,%s,%d,%d,%d,%zu,%.2f,0,0,0,0,0\n", 
                                iter + 1, test_frames[frame_idx].name, width, height, 24,
                                frame_size, frame_size / (1024.0 * 1024.0));
                    }
                    continue;
                }
                printf(" ready\n");
            }
            
            // Clear shared memory header before each iteration
            shm->magic = 0;
            shm->sequence = 0;
            shm->guest_ack = GUEST_ACK_NONE;
            shm->data_size = 0;
            shm->signature_magic = 0;
            shm->error_code = 0;
            memset((void *)shm->data_sha256, 0, 32);
            __sync_synchronize();
            
            // Generate fresh random data for each iteration
            uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
            generate_random_frame(data_ptr, width, height);
            
            // Calculate SHA256 of the data
            uint8_t expected_hash[32];
            calculate_sha256(data_ptr, frame_size, expected_hash);
            
            // Flush cache to ensure data comes from memory
            flush_cache_range((void *)data_ptr, frame_size);
            
            // Prepare header with signature
            shm->guest_ack = GUEST_ACK_NONE;
            shm->sequence = 0xFFFF + iter;  // Unique sequence for each bandwidth test
            shm->data_size = frame_size;
            shm->signature_magic = MAGIC;
            memcpy((void *)shm->data_sha256, expected_hash, 32);
            shm->magic = MAGIC;
            __sync_synchronize();
            
            printf("  [%d] Starting transfer...", iter + 1);
            fflush(stdout);
            
            // Start timer AFTER data generation and cache flush
            uint64_t start_time = get_time_ns();
            
            // Wait for guest to receive message (timeout after 1 second)
            uint64_t timeout = start_time + 1000000000ULL; // 1 second
            while (shm->guest_ack == GUEST_ACK_NONE) {
                if (get_time_ns() > timeout) {
                    printf(" TIMEOUT (message not received)\n");
                    break;
                }
            }
            
            if (shm->guest_ack == GUEST_ACK_NONE) {
                // Export failed result to CSV
                if (csv) {
                    fprintf(csv, "%d,%s,%d,%d,%d,%zu,%.2f,0,0,0,0,0\n", 
                            iter + 1, test_frames[frame_idx].name, width, height, 24,
                            frame_size, frame_size / (1024.0 * 1024.0));
                }
                continue;
            }
            
            printf(" received, processing...");
            fflush(stdout);
            
            // Wait for guest to complete processing (timeout after 5 seconds)
            timeout = get_time_ns() + 5000000000ULL; // 5 seconds
            while (shm->guest_ack == GUEST_ACK_RECEIVED) {
                if (get_time_ns() > timeout) {
                    printf(" TIMEOUT (processing)\n");
                    break;
                }
            }
            
            uint64_t end_time = get_time_ns();
            
            if (shm->guest_ack == GUEST_ACK_PROCESSED) {
                uint64_t duration_ns = end_time - start_time;
                double duration_s = duration_ns / 1e9;
                double bandwidth_mbps = (frame_size / (1024.0 * 1024.0)) / duration_s;
                double bandwidth_gbps = bandwidth_mbps / 1024.0;
                
                total_bandwidth += bandwidth_gbps;
                successful_tests++;
                
                printf(" %.2f GB/s (%.2f ms)\n", bandwidth_gbps, duration_ns / 1000000.0);
                
                // Export to CSV
                if (csv) {
                    fprintf(csv, "%d,%s,%d,%d,%d,%zu,%.2f,%lu,%.2f,%.2f,%.2f,1\n", 
                            iter + 1, test_frames[frame_idx].name, width, height, 24,
                            frame_size, frame_size / (1024.0 * 1024.0),
                            duration_ns, duration_ns / 1000000.0,
                            bandwidth_mbps, bandwidth_gbps);
                }
            } else if (shm->guest_ack == GUEST_ACK_ERROR) {
                printf(" FAILED (guest reported error: %u)\n", shm->error_code);
                if (csv) {
                    fprintf(csv, "%d,%s,%d,%d,%d,%zu,%.2f,0,0,0,0,0\n", 
                            iter + 1, test_frames[frame_idx].name, width, height, 24,
                            frame_size, frame_size / (1024.0 * 1024.0));
                }
            } else {
                printf(" TIMEOUT (processing)\n");
                if (csv) {
                    fprintf(csv, "%d,%s,%d,%d,%d,%zu,%.2f,0,0,0,0,0\n", 
                            iter + 1, test_frames[frame_idx].name, width, height, 24,
                            frame_size, frame_size / (1024.0 * 1024.0));
                }
            }
            
            // Small delay between iterations
            usleep(100000); // 100ms
        }
        
        if (successful_tests > 0) {
            printf("  Average bandwidth: %.2f GB/s (%d/%d successful)\n", 
                   total_bandwidth / successful_tests, successful_tests, iterations);
        } else {
            printf("  No successful transfers for %s\n", test_frames[frame_idx].name);
        }
    }
    
    if (csv) {
        fclose(csv);
        printf("\n  ✓ Bandwidth data exported to bandwidth_results.csv\n");
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
    
    test_bandwidth(shm, 10); // 10 iterations per frame size
    
    // Cleanup
    munmap(ptr, st.st_size);
    close(fd);
    
    printf("\nTests completed.\n");
    return 0;
}

