/*
 * host_writer.c - Host program to write to shared memory
 * 
 * This program writes data to /dev/shm/ivshmem and measures performance
 * with detailed timing breakdown of all overheads
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
                                     uint64_t write_ns, uint64_t roundtrip_ns, 
                                     uint64_t guest_read_ns, uint64_t guest_verify_ns,
                                     bool success)
{
    if (logger && logger->file) {
        double size_mb = size_bytes / (1024.0 * 1024.0);
        double write_bw = success && write_ns > 0 ? (size_mb / (write_ns / 1e9)) : 0.0;
        double read_bw = success && guest_read_ns > 0 ? (size_mb / (guest_read_ns / 1e9)) : 0.0;
        uint64_t total_ns = write_ns + roundtrip_ns;
        double total_bw = success && total_ns > 0 ? (size_mb / (total_ns / 1e9)) : 0.0;
        
        fprintf(logger->file, "%d,%s,%d,%d,%d,%zu,%.2f,%lu,%.2f,%.2f,%lu,%.2f,%lu,%.2f,%.2f,%lu,%.2f,%lu,%.2f,%.2f,%d\n",
                iteration, frame_name, width, height, bpp, size_bytes, size_mb,
                write_ns, write_ns / 1000000.0, write_bw,
                roundtrip_ns, roundtrip_ns / 1000000.0,
                guest_read_ns, guest_read_ns / 1000000.0, read_bw,
                guest_verify_ns, guest_verify_ns / 1000000.0,
                total_ns, total_ns / 1000000.0, total_bw,
                success ? 1 : 0);
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
    printf("\n=== Latency Test with Detailed Overhead Breakdown ===\n");
    printf("Measuring %d messages with 4K frame data...\n", iterations);
    printf("Breakdown: Write (host) → Round-trip (notification + guest processing)\n");
    printf("  Guest reports: Read time + Verify time separately\n\n");
    
    // Create CSV logger with detailed timing columns
    csv_logger_t *csv = csv_create("latency_results.csv", 
        "iteration,write_ns,write_us,roundtrip_ns,roundtrip_us,guest_read_ns,guest_read_us,guest_verify_ns,guest_verify_us,notification_est_ns,notification_est_us,total_ns,total_us,success");
    
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
    
    // Accumulators for statistics
    uint64_t total_write = 0, total_roundtrip = 0, total_read = 0, total_verify = 0, total_notification = 0, total_total = 0;
    uint64_t min_write = UINT64_MAX, max_write = 0;
    uint64_t min_roundtrip = UINT64_MAX, max_roundtrip = 0;
    uint64_t min_read = UINT64_MAX, max_read = 0;
    uint64_t min_verify = UINT64_MAX, max_verify = 0;
    uint64_t min_notification = UINT64_MAX, max_notification = 0;
    int successful = 0;
    
    for (int i = 0; i < iterations; i++) {
        // Clear timing structure
        memset((void *)&shm->timing, 0, sizeof(struct timing_data));
        
        // Reset error code
        shm->error_code = 0;
        __sync_synchronize();
        
        uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
        
        // MEASUREMENT 1: Write time on HOST clock (data generation + hash calculation)
        uint64_t write_start = get_time_ns();
        
        // Generate fresh random frame data
        generate_random_frame(data_ptr, width, height);
        
        // Calculate SHA256 of the data
        uint8_t expected_hash[32];
        calculate_sha256(data_ptr, frame_size, expected_hash);
        
        uint64_t write_end = get_time_ns();
        
        // Prepare message headers
        shm->sequence = i;
        shm->data_size = frame_size;
        memcpy((void*)shm->data_sha256, expected_hash, 32);
        __sync_synchronize();
        
        // MEASUREMENT 2: Round-trip time on HOST clock (from state change to guest done)
        uint64_t roundtrip_start = get_time_ns();
        
        // STATE: HOST_STATE_READY -> HOST_STATE_SENDING
        set_host_state(shm, HOST_STATE_SENDING);
        
        // Wait for guest to start processing (GUEST_STATE_PROCESSING)
        if (!wait_for_guest_state(shm, GUEST_STATE_PROCESSING, 1000000000ULL, "guest processing")) {
            printf("  [%d] TIMEOUT (guest didn't start processing)\n", i);
            if (csv && csv->file) {
                fprintf(csv->file, "%d,0,0,0,0,0,0,0,0,0,0,0,0,0\n", i);
            }
            continue;
        }
        
        // Wait for guest to finish processing (GUEST_STATE_ACKNOWLEDGED)
        if (!wait_for_guest_state(shm, GUEST_STATE_ACKNOWLEDGED, 10000000000ULL, "guest acknowledged")) {
            printf("  [%d] TIMEOUT (guest didn't finish processing)\n", i);
            if (csv && csv->file) {
                fprintf(csv->file, "%d,0,0,0,0,0,0,0,0,0,0,0,0,0\n", i);
            }
            continue;
        }
        
        uint64_t roundtrip_end = get_time_ns();
        
        // Check for errors
        if (shm->error_code != 0) {
            printf("  [%d] ERROR: %u\n", i, shm->error_code);
            if (csv && csv->file) {
                fprintf(csv->file, "%d,0,0,0,0,0,0,0,0,0,0,0,0,0\n", i);
            }
            continue;
        }
        
        // Calculate times (all on HOST clock except guest durations)
        uint64_t write_time = write_end - write_start;
        uint64_t roundtrip_time = roundtrip_end - roundtrip_start;
        
        // Read guest-measured durations (measured on GUEST clock - these are durations, not timestamps)
        uint64_t guest_read_time = shm->timing.guest_read_duration;
        uint64_t guest_verify_time = shm->timing.guest_verify_duration;
        uint64_t guest_total_time = shm->timing.guest_total_duration;
        
        // Estimate notification time = roundtrip - guest_total
        // This is the "overhead" of notification and state machine coordination
        uint64_t notification_est = (roundtrip_time > guest_total_time) ? 
                                    (roundtrip_time - guest_total_time) : 0;
        
        uint64_t total_time = write_time + roundtrip_time;
        
        // Update statistics
        total_write += write_time;
        total_roundtrip += roundtrip_time;
        total_read += guest_read_time;
        total_verify += guest_verify_time;
        total_notification += notification_est;
        total_total += total_time;
        
        if (write_time < min_write) min_write = write_time;
        if (write_time > max_write) max_write = write_time;
        if (roundtrip_time < min_roundtrip) min_roundtrip = roundtrip_time;
        if (roundtrip_time > max_roundtrip) max_roundtrip = roundtrip_time;
        if (guest_read_time < min_read) min_read = guest_read_time;
        if (guest_read_time > max_read) max_read = guest_read_time;
        if (guest_verify_time < min_verify) min_verify = guest_verify_time;
        if (guest_verify_time > max_verify) max_verify = guest_verify_time;
        if (notification_est < min_notification) min_notification = notification_est;
        if (notification_est > max_notification) max_notification = notification_est;
        
        successful++;
        
        // Write to CSV
        if (csv && csv->file) {
            fprintf(csv->file, "%d,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%d\n",
                    i, 
                    write_time, write_time / 1000.0,
                    roundtrip_time, roundtrip_time / 1000.0,
                    guest_read_time, guest_read_time / 1000.0,
                    guest_verify_time, guest_verify_time / 1000.0,
                    notification_est, notification_est / 1000.0,
                    total_time, total_time / 1000.0,
                    1);
        }
        
        if (successful % 100 == 0 || iterations <= 10) {
            printf("  [%d] Write: %.2f µs | RT: %.2f µs (notify~%.2f, read %.2f, verify %.2f) | Total: %.2f µs\n", 
                   i, write_time / 1000.0, roundtrip_time / 1000.0,
                   notification_est / 1000.0, guest_read_time / 1000.0, guest_verify_time / 1000.0,
                   total_time / 1000.0);
        }
        
        // STATE: HOST_STATE_SENDING -> HOST_STATE_READY
        set_host_state(shm, HOST_STATE_READY);
        
        // Wait for guest to be ready for next message
        if (!wait_for_guest_state(shm, GUEST_STATE_READY, 1000000000ULL, "guest ready for next")) {
            printf("  [%d] WARNING: Guest didn't return to ready state\n", i);
        }
    }
    
    if (successful > 0) {
        printf("\n=== Latency Test Results ===\n");
        printf("Successful: %d/%d\n", successful, iterations);
        printf("Frame size: %.2f MB (4K frame)\n\n", frame_size / (1024.0 * 1024.0));
        
        printf("OVERHEAD BREAKDOWN (Average):\n");
        printf("  Write (host):         %7lu ns (%7.2f µs) [%6.1f%%]\n", 
               total_write / successful, (total_write / successful) / 1000.0,
               100.0 * total_write / total_total);
        printf("  Notification (est):   %7lu ns (%7.2f µs) [%6.1f%%]\n", 
               total_notification / successful, (total_notification / successful) / 1000.0,
               100.0 * total_notification / total_total);
        printf("  Read (guest):         %7lu ns (%7.2f µs) [%6.1f%%]\n", 
               total_read / successful, (total_read / successful) / 1000.0,
               100.0 * total_read / total_total);
        printf("  Verify (guest):       %7lu ns (%7.2f µs) [%6.1f%%]\n", 
               total_verify / successful, (total_verify / successful) / 1000.0,
               100.0 * total_verify / total_total);
        printf("  ─────────────────────────────────────────────\n");
        printf("  Total end-to-end:     %7lu ns (%7.2f µs) [100.0%%]\n\n", 
               total_total / successful, (total_total / successful) / 1000.0);
        
        printf("MIN/MAX:\n");
        printf("  Write:        %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_write, max_write, min_write / 1000.0, max_write / 1000.0);
        printf("  Round-trip:   %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_roundtrip, max_roundtrip, min_roundtrip / 1000.0, max_roundtrip / 1000.0);
        printf("  Notification: %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_notification, max_notification, min_notification / 1000.0, max_notification / 1000.0);
        printf("  Read:         %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_read, max_read, min_read / 1000.0, max_read / 1000.0);
        printf("  Verify:       %lu - %lu ns (%.2f - %.2f µs)\n", 
               min_verify, max_verify, min_verify / 1000.0, max_verify / 1000.0);
        
        printf("\nNote: Notification time is estimated as (round-trip - guest_total)\n");
        printf("      Includes polling delay and state machine overhead\n");
    } else {
        printf("\nNo successful measurements. Is the guest program running?\n");
    }
    
    csv_close(csv);
}

void test_bandwidth(volatile struct shared_data *shm, int iterations)
{
    printf("\n=== Bandwidth Test with Overhead Breakdown ===\n");
    printf("Testing memory write/read bandwidth with fresh random data...\n");
    printf("Breakdown: Write BW (host) | Read BW (guest) | Verify Time (guest)\n\n");
    
    // Calculate available buffer size
    size_t header_size = offsetof(struct shared_data, buffer);
    size_t max_data_size = SHMEM_SIZE - header_size;
    
    // Test different frame sizes
    struct {
        int width, height, bpp;
        const char *name;
    } test_frames[] = {
        {1920, 1080, 3, "1080p"},
        {2560, 1440, 3, "1440p"},
        {3840, 2160, 3, "4K"},
        {0, 0, 0, NULL}
    };
    
    // Create CSV logger
    csv_logger_t *csv = csv_create("bandwidth_results.csv", 
        "iteration,frame_type,width,height,bpp,size_bytes,size_mb,write_ns,write_ms,write_mbps,roundtrip_ns,roundtrip_ms,guest_read_ns,guest_read_ms,read_mbps,guest_verify_ns,guest_verify_ms,total_ns,total_ms,total_mbps,success");
    
    for (int frame_idx = 0; test_frames[frame_idx].name != NULL; frame_idx++) {
        int width = test_frames[frame_idx].width;
        int height = test_frames[frame_idx].height;
        int bpp = test_frames[frame_idx].bpp;
        size_t frame_size = width * height * bpp;
        
        if (frame_size > max_data_size) {
            printf("Skipping %s (%dx%d): frame too large\n", 
                   test_frames[frame_idx].name, width, height);
            continue;
        }
        
        printf("\n--- Testing %s (%dx%d, %.2f MB) ---\n", 
               test_frames[frame_idx].name, width, height, frame_size / (1024.0 * 1024.0));
        
        double total_write_bw = 0.0, total_read_bw = 0.0, total_overall_bw = 0.0;
        int successful = 0;
        
        for (int iter = 0; iter < iterations; iter++) {
            if (iter > 0) usleep(10000);
            
            // Clear timing
            memset((void *)&shm->timing, 0, sizeof(struct timing_data));
            shm->error_code = 0;
            __sync_synchronize();
            
            uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
            
            // MEASURE: Write bandwidth on HOST clock
            uint64_t write_start = get_time_ns();
            generate_random_frame(data_ptr, width, height);
            uint8_t expected_hash[32];
            calculate_sha256(data_ptr, frame_size, expected_hash);
            uint64_t write_end = get_time_ns();
            
            shm->sequence = 0xFFFF + iter;
            shm->data_size = frame_size;
            memcpy((void *)shm->data_sha256, expected_hash, 32);
            __sync_synchronize();
            
            // MEASURE: Round-trip time on HOST clock
            uint64_t roundtrip_start = get_time_ns();
            set_host_state(shm, HOST_STATE_SENDING);
            
            if (!wait_for_guest_state(shm, GUEST_STATE_PROCESSING, 2000000000ULL, "guest processing")) {
                printf("  [%d] TIMEOUT\n", iter + 1);
                csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name,
                                         width, height, 24, frame_size, 0, 0, 0, 0, false);
                continue;
            }
            
            if (!wait_for_guest_state(shm, GUEST_STATE_ACKNOWLEDGED, 10000000000ULL, "guest acknowledged")) {
                printf("  [%d] TIMEOUT (processing)\n", iter + 1);
                csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name,
                                         width, height, 24, frame_size, 0, 0, 0, 0, false);
                continue;
            }
            
            uint64_t roundtrip_end = get_time_ns();
            
            if (shm->error_code != 0) {
                printf("  [%d] FAILED (error: %u)\n", iter + 1, shm->error_code);
                csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name,
                                         width, height, 24, frame_size, 0, 0, 0, 0, false);
                continue;
            }
            
            // Calculate times and bandwidths
            uint64_t write_time = write_end - write_start;
            uint64_t roundtrip_time = roundtrip_end - roundtrip_start;
            uint64_t guest_read_time = shm->timing.guest_read_duration;
            uint64_t guest_verify_time = shm->timing.guest_verify_duration;
            uint64_t total_time = write_time + roundtrip_time;
            
            double size_mb = frame_size / (1024.0 * 1024.0);
            double write_bw = size_mb / (write_time / 1e9);
            double read_bw = size_mb / (guest_read_time / 1e9);
            double total_bw = size_mb / (total_time / 1e9);
            
            total_write_bw += write_bw;
            total_read_bw += read_bw;
            total_overall_bw += total_bw;
            successful++;
            
            printf("  [%d] Write: %.0f MB/s | Read: %.0f MB/s | Verify: %.1f ms | Total: %.0f MB/s\n",
                   iter + 1, write_bw, read_bw, guest_verify_time / 1000000.0, total_bw);
            
            csv_write_bandwidth_result(csv, iter + 1, test_frames[frame_idx].name,
                                     width, height, 24, frame_size, 
                                     write_time, roundtrip_time, 
                                     guest_read_time, guest_verify_time, true);
            
            set_host_state(shm, HOST_STATE_READY);
            
            if (!wait_for_guest_state(shm, GUEST_STATE_READY, 1000000000ULL, "guest ready")) {
                printf("  WARNING: Guest didn't return to ready\n");
            }
            
            usleep(100000);
        }
        
        if (successful > 0) {
            printf("\n  %s Results (%d/%d successful):\n", test_frames[frame_idx].name, successful, iterations);
            printf("    Avg Write BW:   %.0f MB/s (%.2f GB/s)\n", 
                   total_write_bw / successful, (total_write_bw / successful) / 1024.0);
            printf("    Avg Read BW:    %.0f MB/s (%.2f GB/s)\n", 
                   total_read_bw / successful, (total_read_bw / successful) / 1024.0);
            printf("    Avg Overall BW: %.0f MB/s (%.2f GB/s)\n", 
                   total_overall_bw / successful, (total_overall_bw / successful) / 1024.0);
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

void init_shared_memory(volatile struct shared_data *shm) {
    printf("HOST: Starting initialization...\n");
    
    guest_state_t guest_state = get_guest_state(shm);
    if (guest_state != GUEST_STATE_UNINITIALIZED) {
        printf("HOST: Detected guest started first (state: %s), clearing...\n", 
               guest_state_name(guest_state));
    }
    
    shm->magic = 0;
    set_host_state(shm, HOST_STATE_INITIALIZING);
    __sync_synchronize();
    
    shm->sequence = 0;
    shm->data_size = 0;
    shm->error_code = 0;
    shm->test_complete = 0;
    memset((void*)shm->data_sha256, 0, 32);
    memset((void*)&shm->timing, 0, sizeof(struct timing_data));
    __sync_synchronize();
    
    shm->magic = MAGIC;
    set_host_state(shm, HOST_STATE_READY);
    __sync_synchronize();
    
    printf("HOST: Initialization complete - waiting for guest...\n");
    
    if (!wait_for_guest_state(shm, GUEST_STATE_READY, 10000000000ULL, "guest ready")) {
        printf("HOST: WARNING - Guest not ready within 10 seconds\n");
        printf("HOST: Current guest state: %s\n", guest_state_name(get_guest_state(shm)));
        printf("HOST: Proceeding anyway...\n");
    } else {
        printf("HOST: ✓ Guest ready - synchronization complete\n");
    }
}

int main(int argc, char *argv[])
{
    bool run_latency = false;
    bool run_bandwidth = false;
    int latency_count = 100;
    int bandwidth_count = 10;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--latency") == 0) {
            run_latency = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                latency_count = atoi(argv[++i]);
                if (latency_count <= 0) latency_count = 1;
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bandwidth") == 0) {
            run_bandwidth = true;
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
    
    if (!run_latency && !run_bandwidth) {
        run_latency = true;
        run_bandwidth = true;
    }
    
    printf("Host Writer - ivshmem Performance Test with Overhead Analysis\n");
    printf("=============================================================\n\n");
    
    int fd = open(SHMEM_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open shared memory");
        printf("Make sure the VM setup script has been run.\n");
        return 1;
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    
    printf("Shared memory: %s (%ld bytes)\n", SHMEM_PATH, st.st_size);
    
    void *ptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    volatile struct shared_data *shm = (volatile struct shared_data *)ptr;
    
    printf("Mapped at address: %p\n", ptr);
    printf("Data buffer size: %zu bytes\n", 
           st.st_size - offsetof(struct shared_data, buffer));
    
    printf("\nInitializing shared memory protocol...\n");
    init_shared_memory(shm);
    
    printf("\nMake sure the guest program is running!\n");
    printf("Press Enter to start tests...\n");
    getchar();
    
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
    
    set_host_state(shm, HOST_STATE_COMPLETED);
    shm->test_complete = 1;
    __sync_synchronize();
    
    munmap(ptr, st.st_size);
    close(fd);
    
    printf("\nTests completed.\n");
    return 0;
}