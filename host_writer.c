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
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>

#include "common.h"

#define SHMEM_PATH "/dev/shm/ivshmem"
#define SHMEM_SIZE (64 * 1024 * 1024)  // 64MB
#define FRAME_SIZE (3840 * 2160 * 4)    // 4K RGBA frame (33MB)

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Debug logging function
static void debug_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("DEBUG: ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

// Host state management (host only modifies host_state)
static void set_host_state(volatile struct shared_data *shm, host_state_t new_state)
{
    host_state_t old_state = (host_state_t)shm->host_state;
    if (old_state != new_state) {
        printf("HOST STATE: %s -> %s\n", host_state_name(old_state), host_state_name(new_state));
        shm->host_state = (uint32_t)new_state;
        __sync_synchronize();
    }
}

static host_state_t get_host_state(volatile struct shared_data *shm)
{
    return (host_state_t)shm->host_state;
}

static guest_state_t get_guest_state(volatile struct shared_data *shm)
{
    return (guest_state_t)shm->guest_state;
}

// No helper functions needed - using POSIX IPC primitives directly

// CSV result logging helper
typedef struct {
    FILE *file;
    const char *filename;
} csv_logger_t;

static csv_logger_t* csv_create(const char *filename, const char *header)
{
    csv_logger_t *logger = malloc(sizeof(csv_logger_t));
    if (!logger) return NULL;
    
    logger->file = fopen(filename, "w");
    logger->filename = filename;
    
    if (logger->file && header) {
        fprintf(logger->file, "%s\n", header);
    }
    
    return logger;
}


static void csv_write_bandwidth_result(csv_logger_t *logger, int iteration, const char *frame_name,
                                     int width, int height, int bpp, size_t size_bytes,
                                     uint64_t duration_ns, bool success)
{
    if (logger && logger->file) {
        double size_mb = size_bytes / (1024.0 * 1024.0);
        double duration_ms = duration_ns / 1000000.0;
        double bandwidth_mbps = success ? (size_mb / (duration_ns / 1e9)) : 0.0;
        double bandwidth_gbps = success ? (bandwidth_mbps / 1024.0) : 0.0;
        
        fprintf(logger->file, "%d,%s,%d,%d,%d,%zu,%.2f,%lu,%.2f,%.2f,%.2f,%d\n",
                iteration, frame_name, width, height, bpp, size_bytes, size_mb,
                duration_ns, duration_ms, bandwidth_mbps, bandwidth_gbps, success ? 1 : 0);
    }
}

static void csv_close(csv_logger_t *logger)
{
    if (logger) {
        if (logger->file) {
            fclose(logger->file);
            printf("\n  ✓ Data exported to %s\n", logger->filename);
        }
        free(logger);
    }
}

// Calculate SHA256 hash of buffer
static void calculate_sha256(const uint8_t *data, size_t len, uint8_t *hash)
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(hash, &ctx);
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

// Wait for guest state change with timeout
static bool wait_for_guest_state(volatile struct shared_data *shm, guest_state_t expected_state, 
                                  uint64_t timeout_ns, const char *description)
{
    uint64_t start_time = get_time_ns();
    
    while (get_guest_state(shm) != expected_state) {
        if (get_time_ns() - start_time > timeout_ns) {
            debug_log("TIMEOUT waiting for guest state %s (current: %s)", 
                     guest_state_name(expected_state), 
                     guest_state_name(get_guest_state(shm)));
            return false;
        }
        usleep(10); // 10 microsecond polling interval
    }
    
    return true;
}

void test_latency(volatile struct shared_data *shm, int iterations)
{
    printf("\n=== Latency Test (State-Based Protocol) ===\n");
    printf("Sending %d messages with 4K frame data and measuring latency...\n", iterations);
    
    // Create CSV logger with enhanced header
    csv_logger_t *csv = csv_create("latency_results.csv", "iteration,latency_ns,latency_us,processing_ns,processing_us");
    
    // Calculate available buffer size and use 4K frame for latency test
    size_t header_size = offsetof(struct shared_data, buffer);
    size_t max_data_size = SHMEM_SIZE - header_size;
    
    // Use 4K frame (3840x2160x3 = 24.8MB) for latency test
    int width = 3840, height = 2160, bpp = 3;
    size_t frame_size = width * height * bpp;
    
    if (frame_size > max_data_size) {
        printf("ERROR: 4K frame too large (%zu bytes > %zu bytes)\n", frame_size, max_data_size);
        csv_close(csv);
        return;
    }
    
    printf("Using 4K frame: %dx%d, %.2f MB per message\n", 
           width, height, frame_size / (1024.0 * 1024.0));
    
    uint64_t total_latency = 0;
    uint64_t total_processing = 0;
    int successful = 0;
    uint64_t min_latency = UINT64_MAX;
    uint64_t max_latency = 0;
    
    for (int i = 0; i < iterations; i++) {
        // PREPARE ALL DATA FIRST - BEFORE changing state!
        
        // Generate fresh random frame data for each iteration
        uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
        generate_random_frame(data_ptr, width, height);
        
        // Calculate SHA256 of the data
        uint8_t expected_hash[32];
        calculate_sha256(data_ptr, frame_size, expected_hash);
        
        // Reset error code
        shm->error_code = 0;
        __sync_synchronize();
        
        // Prepare message headers
        shm->sequence = i;
        shm->data_size = frame_size; // Latency test uses FULL 4K frame!
        memcpy((void*)shm->data_sha256, expected_hash, 32);
        __sync_synchronize();
        
        // NOW signal that data is ready - STATE CHANGE LAST!
        // STATE: HOST_STATE_READY -> HOST_STATE_SENDING
        set_host_state(shm, HOST_STATE_SENDING);
        
        // Start timer
        uint64_t start_time = get_time_ns();
        
        // Wait for guest to start processing (GUEST_STATE_PROCESSING)
        if (!wait_for_guest_state(shm, GUEST_STATE_PROCESSING, 1000000000ULL, "guest processing")) {
            printf("  [%d] TIMEOUT (guest didn't start processing)\n", i);
            continue;
        }
        
        // Stop latency timer - this measures true latency (time to start processing)
        uint64_t latency_end_time = get_time_ns();
        
        // Wait for guest to finish processing (GUEST_STATE_ACKNOWLEDGED)
        if (!wait_for_guest_state(shm, GUEST_STATE_ACKNOWLEDGED, 1000000000ULL, "guest acknowledged")) {
            printf("  [%d] TIMEOUT (guest didn't finish processing)\n", i);
            // Still count the latency measurement since guest started processing
        }
        
        uint64_t processing_end_time = get_time_ns();
        
        // Check for errors
        if (shm->error_code != 0) {
            printf("  [%d] ERROR: %u\n", i, shm->error_code);
            continue;
        }
        
        // Record successful measurement
        uint64_t latency = latency_end_time - start_time;
        uint64_t processing_time = processing_end_time - start_time;
        
        // Write to CSV with both latency and processing time
        if (csv && csv->file) {
            fprintf(csv->file, "%d,%lu,%.2f,%lu,%.2f\n", 
                   successful, latency, latency / 1000.0, 
                   processing_time, processing_time / 1000.0);
        }
        
        total_latency += latency;
        total_processing += processing_time;
        successful++;
        
        if (latency < min_latency) min_latency = latency;
        if (latency > max_latency) max_latency = latency;
        
        if (successful % 100 == 0 || iterations <= 10) {
            printf("  [%d] Latency: %lu ns (%.2f µs), Processing: %lu ns (%.2f µs)\n", 
                   i, latency, latency / 1000.0, processing_time, processing_time / 1000.0);
        }
        
        // STATE: HOST_STATE_SENDING -> HOST_STATE_READY (ready for next message)
        set_host_state(shm, HOST_STATE_READY);
        
        // Wait for guest to be ready for next message
        if (!wait_for_guest_state(shm, GUEST_STATE_READY, 1000000000ULL, "guest ready for next")) {
            printf("  [%d] WARNING: Guest didn't return to ready state\n", i);
        }
    }
    
    if (successful > 0) {
        printf("\nResults:\n");
        printf("  Successful: %d/%d\n", successful, iterations);
        printf("  Frame size: %.2f MB (4K frame)\n", frame_size / (1024.0 * 1024.0));
        printf("  Average latency (receive): %lu ns (%.2f µs)\n", 
               total_latency / successful, 
               (total_latency / successful) / 1000.0);
        printf("  Average processing time: %lu ns (%.2f µs)\n", 
               total_processing / successful, 
               (total_processing / successful) / 1000.0);
        printf("  Min latency: %lu ns (%.2f µs)\n", 
               min_latency, min_latency / 1000.0);
        printf("  Max latency: %lu ns (%.2f µs)\n", 
               max_latency, max_latency / 1000.0);
        
        printf("\n  ✓ Data exported to latency_results.csv\n");
    } else {
        printf("\nNo successful measurements. Is the guest program running?\n");
    }
    
    csv_close(csv);
}

void test_bandwidth(volatile struct shared_data *shm, int iterations)
{
    printf("\n=== Bandwidth Test ===\n");
    printf("Testing transmission times with fresh random data per iteration...\n");
    printf("Guest will handle cache flushing to ensure real memory reads.\n");
    
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
    
    // Create CSV logger
    csv_logger_t *csv = csv_create("bandwidth_results.csv", 
        "iteration,frame_type,width,height,bpp,test_size_bytes,test_size_mb,duration_ns,duration_ms,bandwidth_mbps,bandwidth_gbps,data_verified");
    
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
            // Small delay between iterations
            if (iter > 0) {
                usleep(10000); // 10ms delay
            }
            
            // PREPARE ALL DATA FIRST - BEFORE timing starts
            
            // Generate fresh random data for each iteration
            uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
            generate_random_frame(data_ptr, width, height);
            
            // Calculate SHA256 of the data
            uint8_t expected_hash[32];
            calculate_sha256(data_ptr, frame_size, expected_hash);
            
            // Reset error code
            shm->error_code = 0;
            __sync_synchronize();
            
            // Prepare header
            shm->sequence = 0xFFFF + iter;  // Unique sequence for each bandwidth test
            shm->data_size = frame_size;
            memcpy((void *)shm->data_sha256, expected_hash, 32);
            __sync_synchronize();
            
            // START TIMER - measuring transmission time only
            uint64_t start_time = get_time_ns();
            
            // NOW signal that data is ready - STATE CHANGE starts transmission
            // STATE: HOST_STATE_READY -> HOST_STATE_SENDING
            set_host_state(shm, HOST_STATE_SENDING);
            
            printf("  [%d] Starting transfer...", iter + 1);
            fflush(stdout);
            
            // Wait for guest to start processing
            if (!wait_for_guest_state(shm, GUEST_STATE_PROCESSING, 2000000000ULL, "guest processing")) {
                printf(" TIMEOUT (guest didn't start processing)\n");
                csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name, 
                                         width, height, 24, frame_size, 0, false);
                continue;
            }
            
            printf(" received, processing...");
            fflush(stdout);
            
            // Wait for guest to finish processing
            if (!wait_for_guest_state(shm, GUEST_STATE_ACKNOWLEDGED, 10000000000ULL, "guest acknowledged")) {
                printf(" TIMEOUT (processing not finished)\n");
                csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name, 
                                         width, height, 24, frame_size, 0, false);
                continue;
            }
            
            uint64_t end_time = get_time_ns();
            
            // Process results
            uint64_t duration_ns = end_time - start_time;
            
            // Check for errors
            bool success = (shm->error_code == 0);
            
            if (success) {
                double duration_s = duration_ns / 1e9;
                double bandwidth_mbps = (frame_size / (1024.0 * 1024.0)) / duration_s;
                double bandwidth_gbps = bandwidth_mbps / 1024.0;
                
                total_bandwidth += bandwidth_gbps;
                successful_tests++;
                
                printf(" %.2f GB/s (%.2f ms)\n", bandwidth_gbps, duration_ns / 1000000.0);
            } else {
                printf(" FAILED (guest reported error: %u)\n", shm->error_code);
            }
            
            // Log result to CSV
            csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name, 
                                     width, height, 24, frame_size, duration_ns, success);
            
            // STATE: HOST_STATE_SENDING -> HOST_STATE_READY
            set_host_state(shm, HOST_STATE_READY);
            
            // Wait for guest to be ready for next message
            if (!wait_for_guest_state(shm, GUEST_STATE_READY, 1000000000ULL, "guest ready for next")) {
                printf("  WARNING: Guest didn't return to ready state\n");
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
    
    csv_close(csv);
}

void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("Options:\n");
    printf("  -l, --latency [COUNT]     Run latency test (default: 100 messages)\n");
    printf("  -b, --bandwidth [COUNT]   Run bandwidth test (default: 10 iterations)\n");
    printf("  -c, --count COUNT         Number of messages/iterations\n");
    printf("  -h, --help               Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -l 1                  Send single latency message\n", prog_name);
    printf("  %s -l 100                Send 100 latency messages\n", prog_name);
    printf("  %s -b 5                  Run 5 bandwidth iterations\n", prog_name);
    printf("  %s -l -b                 Run both tests with defaults\n", prog_name);
}

// Initialize shared memory with graceful startup handling
void init_shared_memory(volatile struct shared_data *shm) {
    printf("HOST: Starting initialization...\n");
    
    // Check if guest started first and left stale data
    guest_state_t guest_state = get_guest_state(shm);
    if (guest_state != GUEST_STATE_UNINITIALIZED) {
        printf("HOST: Detected guest started first (guest state: %s), clearing stale data...\n", 
               guest_state_name(guest_state));
    }
    
    // STATE: HOST_STATE_INITIALIZING
    // CRITICAL: Clear magic first to signal "initializing"
    shm->magic = 0;
    set_host_state(shm, HOST_STATE_INITIALIZING);
    __sync_synchronize();
    
    // Initialize all fields to known clean state (but don't touch guest_state - that's guest's job)
    shm->sequence = 0;
    shm->data_size = 0;
    shm->error_code = 0;
    shm->test_complete = 0;
    memset((void*)shm->data_sha256, 0, 32);
    __sync_synchronize();
    
    // CRITICAL: Set magic LAST to signal "initialization complete"
    // STATE: HOST_STATE_READY
    shm->magic = MAGIC;
    set_host_state(shm, HOST_STATE_READY);
    __sync_synchronize();
    
    printf("HOST: Initialization complete - waiting for guest to be ready...\n");
    
    // Wait for guest to be ready (with timeout)
    if (!wait_for_guest_state(shm, GUEST_STATE_READY, 10000000000ULL, "guest ready")) {
        printf("HOST: WARNING - Guest didn't reach ready state within 10 seconds\n");
        printf("HOST: Current guest state: %s\n", guest_state_name(get_guest_state(shm)));
        printf("HOST: Proceeding anyway...\n");
    } else {
        printf("HOST: ✓ Guest is ready - synchronization complete\n");
    }
}

int main(int argc, char *argv[])
{
    bool run_latency = false;
    bool run_bandwidth = false;
    int latency_count = 100;
    int bandwidth_count = 10;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--latency") == 0) {
            run_latency = true;
            // Check if next argument is a number
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                latency_count = atoi(argv[++i]);
                if (latency_count <= 0) latency_count = 1;
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bandwidth") == 0) {
            run_bandwidth = true;
            // Check if next argument is a number
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                bandwidth_count = atoi(argv[++i]);
                if (bandwidth_count <= 0) bandwidth_count = 1;
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--count") == 0) {
            if (i + 1 < argc) {
                int count = atoi(argv[++i]);
                if (count > 0) {
                    latency_count = count;
                    bandwidth_count = count;
                }
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // If no tests specified, run both
    if (!run_latency && !run_bandwidth) {
        run_latency = true;
        run_bandwidth = true;
    }
    
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
    
    // Initialize shared memory with custom polling protocol IMMEDIATELY
    printf("\nInitializing shared memory protocol...\n");
    init_shared_memory(shm);
    printf("Shared memory initialization complete.\n");
    printf("Guest programs can now start safely.\n");
    
    // Wait for user
    printf("\nMake sure the guest program is running!\n");
    printf("Press Enter to start tests...\n");
    getchar();
    
    // Run tests
    if (run_latency) {
        test_latency(shm, latency_count);
    }
    
    if (run_bandwidth) {
        if (run_latency) {
            printf("\nPress Enter to run bandwidth test...\n");
            getchar();
        }
        test_bandwidth(shm, bandwidth_count);
    }
    
    // Signal test completion
    set_host_state(shm, HOST_STATE_COMPLETED);
    shm->test_complete = 1;
    __sync_synchronize(); // Ensure completion flag is visible
    munmap(ptr, st.st_size);
    close(fd);
    
    printf("\nTests completed.\n");
    return 0;
}

