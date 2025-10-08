/*
 * guest_reader.c - Guest program to read from ivshmem PCI device
 * 
 * This program reads data from the ivshmem PCI BAR2 and measures performance
 * with detailed timing breakdown of all overheads.
 * 
 * SIMD OPTIMIZATION:
 * The data read loops include compiler hints for SIMD vectorization:
 * - #pragma GCC ivdep: Ignore vector dependencies 
 * - #pragma clang loop vectorize(enable): Force Clang vectorization
 * - #pragma GCC unroll 4: Unroll loops 4x for better instruction pipelining
 * - __restrict__ pointers: Help compiler understand memory aliasing
 * - __builtin_assume_aligned(): Alignment hints for vectorization
 * 
 * Compile with: gcc -O2 -march=native -ftree-vectorize
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
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>

#include "common.h"
#include "performance_counters.h"

#define PCI_RESOURCE_PATH "/sys/bus/pci/devices/0000:00:03.0/resource2"
#define PCI_RESOURCE_WC_PATH "/sys/bus/pci/devices/0000:00:03.0/resource2_wc"
#define SHMEM_PATH "/dev/shm/ivshmem"

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Debug logging helper
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

// Guest state management
static void set_guest_state(volatile struct shared_data *shm, guest_state_t new_state)
{
    guest_state_t old_state = (guest_state_t)shm->guest_state;
    if (old_state != new_state) {
        printf("GUEST STATE: %s -> %s\n", guest_state_name(old_state), guest_state_name(new_state));
        shm->guest_state = (uint32_t)new_state;
        __sync_synchronize();
    }
}

static guest_state_t get_guest_state(volatile struct shared_data *shm)
{
    return (guest_state_t)shm->guest_state;
}

static host_state_t get_host_state(volatile struct shared_data *shm)
{
    return (host_state_t)shm->host_state;
}

// Verify data integrity using SHA256
static bool verify_data_integrity(const uint8_t *data, uint32_t size, const uint8_t *expected_hash)
{
    uint8_t calculated_hash[32];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, size);
    SHA256_Final(calculated_hash, &ctx);
    
    return memcmp(calculated_hash, expected_hash, 32) == 0;
}

// Print hash comparison for debugging
static void print_hash_comparison(const uint8_t *expected, const uint8_t *calculated)
{
    printf("  Expected: ");
    for (int i = 0; i < 32; i++) printf("%02x", expected[i]);
    printf("\n  Got:      ");
    for (int i = 0; i < 32; i++) printf("%02x", calculated[i]);
    printf("\n");
}

// Cache flush function
static void flush_cache_range(void *addr, size_t len) {
    #if defined(__x86_64__) || defined(__i386__)
    char *ptr = (char *)addr;
    for (size_t i = 0; i < len; i += 64) {
        __builtin_ia32_clflush(ptr + i);
    }
    __sync_synchronize();
    #else
    __sync_synchronize();
    #endif
}

// Force read of buffer (prevents optimization while allowing realistic performance)
static void force_buffer_read(const uint8_t *data, uint32_t size)
{
    uint64_t dummy = 0;
    uint64_t * __restrict__ data64 = (uint64_t * __restrict__)data;
    size_t size64 = size / 8;
    size_t remainder = size % 8;
    
    // Hint to compiler for alignment and vectorization
    data64 = (uint64_t *)__builtin_assume_aligned(data64, 8);
    
    // SIMD optimization hints for vectorized buffer read
    #pragma GCC ivdep
    #pragma clang loop vectorize(enable)
    #pragma GCC unroll 4
    for (size_t i = 0; i < size64; i++) {
        dummy ^= data64[i];
    }
    
    // Handle remainder bytes (non-vectorized)
    for (size_t i = size64 * 8; i < size64 * 8 + remainder; i++) {
        dummy ^= data[i];
    }
    
    // Use result to prevent optimization
    if ((dummy & 0xFF) == 0xFF) {
        printf("Unusual: checksum LSB was 0xFF\n");
    }
}

// Print usage
static void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Options:\n");
    printf("  -l, --latency [COUNT]     Expect latency test (default: 100 messages)\n");
    printf("  -b, --bandwidth [COUNT]   Expect bandwidth test (default: 10 iterations)\n");
    printf("  -c, --count COUNT         Number of messages/iterations to expect\n");
    printf("  -w, --write-combining     Use resource2_wc (write-combining) instead of resource2\n");
    printf("  -h, --help               Show this help\n");
    printf("\n");
}

// Initialize guest communication and wait for host handshake
static bool initialize_guest_communication(volatile struct shared_data *shm)
{
    printf("GUEST: Checking for host initialization...\n");
    
    host_state_t host_state = get_host_state(shm);
    if (host_state != HOST_STATE_UNINITIALIZED && host_state != HOST_STATE_READY) {
        printf("GUEST: Detected host in state %s, waiting...\n", host_state_name(host_state));
    }
    
    printf("GUEST: Waiting for host initialization handshake...\n");
    
    int init_timeout = 0;
    bool host_ready = false;
    
    while (!host_ready && init_timeout < 5000) {
        if (shm->magic == MAGIC && get_host_state(shm) == HOST_STATE_READY) {
            host_ready = true;
            break;
        }
        
        if (shm->magic == 0 && get_host_state(shm) == HOST_STATE_INITIALIZING) {
            printf("GUEST: Host initialization in progress...\n");
        }
        
        usleep(10000);
        init_timeout++;
    }
    
    if (!host_ready) {
        printf("GUEST: TIMEOUT - Host not ready after 50 seconds\n");
        printf("GUEST: Current state - Magic: 0x%08X, Host state: %s\n", 
               shm->magic, host_state_name(get_host_state(shm)));
        return false;
    }
    
    printf("GUEST: ✓ Host initialization complete - ready for messages.\n\n");
    return true;
}

// Setup and initialize performance counters
static bool setup_performance_counters(struct perf_counters *perf_counters)
{
    bool perf_available = perf_counters_init(perf_counters);
    if (perf_available) {
        printf("GUEST: ✓ Hardware performance counters initialized\n\n");
    } else {
        printf("GUEST: ⚠ Hardware performance counters not available\n");
        printf("  Cache miss analysis will be limited\n\n");
    }
    return perf_available;
}

// Structure to hold timing results for a message
struct message_timing {
    uint64_t hot_cache_duration;
    uint64_t cold_cache_duration;
    uint64_t second_pass_duration;
    uint64_t cached_verify_duration;
    uint64_t total_duration;
    struct perf_results perf_results;
};

// Perform memory access tests on the shared memory data
static void run_memory_access_tests(uint8_t *data_ptr, uint32_t data_size, uint8_t *measurement_buffer,
                                   bool perf_available, struct perf_counters *perf_counters,
                                   struct message_timing *timing)
{
    // Start performance counters for all measurements
    if (perf_available) {
        perf_counters_start(perf_counters);
    }
    
    // Setup for SIMD-optimized 64-bit reads
    uint64_t * __restrict__ data_ptr64 = (uint64_t * __restrict__)data_ptr;
    size_t size64 = data_size / 8;
    size_t remainder = data_size % 8;
    
    // Hint to compiler that data_ptr64 is aligned for better vectorization
    data_ptr64 = (uint64_t *)__builtin_assume_aligned(data_ptr64, 8);
    
    // WARM-UP: Initial access to handle page faults and system overhead
    // This is not measured but prepares the system for accurate measurements
    uint64_t dummy_warmup = 0;
    
    // SIMD optimization hints for vectorization
    #pragma GCC ivdep
    #pragma clang loop vectorize(enable)
    #pragma GCC unroll 4
    for (size_t i = 0; i < size64; i++) {
        dummy_warmup ^= data_ptr64[i];
    }
    
    // Handle remainder bytes (non-vectorized)
    for (size_t i = size64 * 8; i < size64 * 8 + remainder; i++) {
        dummy_warmup ^= data_ptr[i];
    }
    __sync_synchronize();
    
    // PHASE A: PURE READ (HOT CACHE) - Read shared memory without writing
    // After warm-up, data should be in CPU cache
    uint64_t hot_read_start = get_time_ns();
    
    uint64_t dummy_hot = 0;
    
    // SIMD optimization hints for vectorized hot cache read
    #pragma GCC ivdep
    #pragma clang loop vectorize(enable)
    #pragma GCC unroll 4
    for (size_t i = 0; i < size64; i++) {
        dummy_hot ^= data_ptr64[i];
    }
    
    // Handle remainder bytes (non-vectorized)
    for (size_t i = size64 * 8; i < size64 * 8 + remainder; i++) {
        dummy_hot ^= data_ptr[i];
    }
    __sync_synchronize(); // Ensure reads complete
    
    uint64_t hot_read_end = get_time_ns();
    timing->hot_cache_duration = hot_read_end - hot_read_start;
    
    // PHASE B: PURE READ (COLD CACHE) - Read shared memory after cache flush
    // Flush cache lines for the shared memory to force memory access
    flush_cache_range(data_ptr, data_size);
    
    uint64_t cold_read_start = get_time_ns();
    
    uint64_t dummy_cold = 0;
    
    // SIMD optimization hints for vectorized cold cache read  
    #pragma GCC ivdep
    #pragma clang loop vectorize(enable)
    #pragma GCC unroll 4
    for (size_t i = 0; i < size64; i++) {
        dummy_cold ^= data_ptr64[i];
    }
    
    // Handle remainder bytes (non-vectorized)
    for (size_t i = size64 * 8; i < size64 * 8 + remainder; i++) {
        dummy_cold ^= data_ptr[i];
    }
    __sync_synchronize(); // Ensure reads complete
    
    uint64_t cold_read_end = get_time_ns();
    timing->cold_cache_duration = cold_read_end - cold_read_start;
    
    // PHASE C: READ+WRITE (COLD CACHE) - memcpy after cache flush to measure write overhead
    // Flush cache again to ensure we're measuring from cold state
    flush_cache_range(data_ptr, data_size);
    
    uint64_t memcpy_start = get_time_ns();
    
    memcpy(measurement_buffer, data_ptr, data_size);
    __sync_synchronize(); // Ensure memcpy completes
    
    uint64_t memcpy_end = get_time_ns();
    timing->second_pass_duration = memcpy_end - memcpy_start;
    
    // Stop performance counters after all memory operations
    if (perf_available) {
        perf_counters_stop(perf_counters, &timing->perf_results, data_size * 4); // warmup + 2 reads + 1 memcpy
    }
    
    // Prevent compiler optimization of read loops by using the computed values
    // This ensures all reads actually happen without artificial constraints
    if (dummy_warmup + dummy_hot + dummy_cold == 0) {
        printf("Impossible: all data was zero (this prevents optimization)\n");
    }
}

// Perform data integrity verification with timing
static bool verify_data_with_timing(const uint8_t *data, uint32_t data_size, 
                                   const uint8_t *expected_hash, uint64_t *verify_duration)
{
    uint64_t verify_start = get_time_ns();
    
    bool hash_match = verify_data_integrity(data, data_size, expected_hash);
    
    uint64_t verify_end = get_time_ns();
    *verify_duration = verify_end - verify_start;
    
    return hash_match;
}

// Display performance results and analysis
static void display_performance_results(uint32_t data_size, const struct message_timing *timing,
                                       bool perf_available, bool use_write_combining, bool hash_match)
{
    // Calculate legacy timing for backward compatibility
    uint64_t memcpy_duration = timing->second_pass_duration;
    
    // Display results with performance metrics
    printf("Guest Timing (measured on guest clock) - %s Analysis:\n", 
           use_write_combining ? "Write-Combining" : "Standard");
    printf("  Phase A (Pure Read Hot):   %lu ns (%.2f µs) [%6.0f MB/s] - 64-bit read+XOR (hot cache)\n", 
           timing->hot_cache_duration, timing->hot_cache_duration / 1000.0, 
           (data_size / (1024.0 * 1024.0)) / (timing->hot_cache_duration / 1e9));
    printf("  Phase B (Pure Read Cold):  %lu ns (%.2f µs) [%6.0f MB/s] - 64-bit read+XOR (cold cache)\n", 
           timing->cold_cache_duration, timing->cold_cache_duration / 1000.0, 
           (data_size / (1024.0 * 1024.0)) / (timing->cold_cache_duration / 1e9));
    printf("  Phase C (Read+Write):      %lu ns (%.2f µs) [%6.0f MB/s] - memcpy (%s)\n", 
           timing->second_pass_duration, timing->second_pass_duration / 1000.0, 
           (data_size / (1024.0 * 1024.0)) / (timing->second_pass_duration / 1e9),
           use_write_combining ? "write-combining" : "standard caching");
    printf("  Phase D (SHA256 Verify):   %lu ns (%.2f µs) [testing only] - Integrity check\n", 
           timing->cached_verify_duration, timing->cached_verify_duration / 1000.0);
    
    if (perf_available) {
        printf("    Performance (2 reads + 1 memcpy):  L1 cache %.1f%% miss, LLC cache %.1f%% miss, TLB %.3f%% miss\n",
               timing->perf_results.l1_cache_miss_rate * 100.0,
               timing->perf_results.llc_cache_miss_rate * 100.0,
               timing->perf_results.tlb_miss_rate * 100.0);
        printf("    CPU:          %.2f IPC, %.1f cycles/byte, %lu context switches\n",
               timing->perf_results.instructions_per_cycle,
               timing->perf_results.cycles_per_byte,
               timing->perf_results.context_switches);
    }
    
    printf("\nAnalysis:\n");
    printf("  Write overhead (C-B):  %+ld ns (%+.2f µs) [%+.1f%%]\n",
           (int64_t)(timing->second_pass_duration - timing->cold_cache_duration),
           (timing->second_pass_duration - timing->cold_cache_duration) / 1000.0,
           ((double)(timing->second_pass_duration - timing->cold_cache_duration) / timing->cold_cache_duration) * 100.0);
    printf("  Cache effect (B-A):    %+ld ns (%+.2f µs) [%+.1f%%]\n",
           (int64_t)(timing->cold_cache_duration - timing->hot_cache_duration),
           (timing->cold_cache_duration - timing->hot_cache_duration) / 1000.0,
           ((double)(timing->cold_cache_duration - timing->hot_cache_duration) / timing->hot_cache_duration) * 100.0);
    printf("  Legacy memcpy:         %lu ns (%.2f µs) [%6.0f MB/s] (Phase C)\n", 
           memcpy_duration, memcpy_duration / 1000.0, 
           (data_size / (1024.0 * 1024.0)) / (memcpy_duration / 1e9));
    printf("  Total:                 %lu ns (%.2f µs)\n", 
           timing->total_duration, timing->total_duration / 1000.0);
    
    if (hash_match) {
        printf("✓ Data integrity verified: SHA256 match\n");
    } else {
        printf("✗ Data integrity check FAILED: SHA256 mismatch\n");
    }
    
    printf("  Processing complete\n\n");
}

// Process a single message from the host
static bool process_single_message(volatile struct shared_data *shm, uint8_t *local_buffer,
                                  bool perf_available, struct perf_counters *perf_counters,
                                  bool use_write_combining)
{
    // Start total processing timer (on GUEST clock)
    uint64_t processing_start = get_time_ns();
    
    // STATE: GUEST_STATE_READY -> GUEST_STATE_PROCESSING
    set_guest_state(shm, GUEST_STATE_PROCESSING);
    
    // Read message metadata
    uint32_t sequence = shm->sequence;
    uint32_t data_size = shm->data_size;
    
    uint8_t expected_hash[32];
    memcpy(expected_hash, (const void *)shm->data_sha256, 32);
    
    printf("Sequence: %u, Data size: %u bytes (%.2f MB)\n", 
           sequence, data_size, data_size / (1024.0 * 1024.0));
    fflush(stdout);
    
    bool success = true;
    uint32_t error_code = 0;
    uint8_t *measurement_buffer = NULL;
    
    // Get data pointer from shared memory
    uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
    
    // Pre-allocate measurement buffer (reuse for consistent measurements)
    measurement_buffer = malloc(data_size);
    if (!measurement_buffer) {
        printf("Error: Failed to allocate measurement buffer\n");
        shm->error_code = 2;
        return false;
    }
    
    // Warm up the measurement buffer to avoid allocation overhead in measurements
    memset(measurement_buffer, 0, data_size);
    
    struct message_timing timing = {0};
    
    // Run the memory access tests
    run_memory_access_tests(data_ptr, data_size, measurement_buffer, perf_available, perf_counters, &timing);
    
    // Copy final data to local buffer for verification (using the memcpy result)
    memcpy(local_buffer, measurement_buffer, data_size);
    
    // PHASE D: SHA256 INTEGRITY CHECK - SHA256 with data in local cache
    bool hash_match = verify_data_with_timing(local_buffer, data_size, expected_hash, &timing.cached_verify_duration);
    
    if (!hash_match) {
        uint8_t calculated_hash[32];
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, local_buffer, data_size);
        SHA256_Final(calculated_hash, &ctx);
        print_hash_comparison(expected_hash, calculated_hash);
        success = false;
        error_code = 1;
    }
    
    // Calculate total processing duration
    uint64_t processing_end = get_time_ns();
    timing.total_duration = processing_end - processing_start;
    
    // WRITE DURATIONS and PERFORMANCE METRICS to shared memory for host to read
    // Legacy fields for backward compatibility
    shm->timing.guest_copy_duration = timing.second_pass_duration;
    shm->timing.guest_verify_duration = timing.cached_verify_duration;
    shm->timing.guest_total_duration = timing.total_duration;
    
    // NEW: Detailed cache behavior measurements
    shm->timing.guest_hot_cache_duration = timing.hot_cache_duration;
    shm->timing.guest_cold_cache_duration = timing.cold_cache_duration;
    shm->timing.guest_second_pass_duration = timing.second_pass_duration;
    shm->timing.guest_cached_verify_duration = timing.cached_verify_duration;
    
    // Write performance metrics (convert floating point to fixed-point for shared memory)
    shm->timing.guest_perf.l1_cache_misses = timing.perf_results.l1_cache_misses;
    shm->timing.guest_perf.l1_cache_references = timing.perf_results.l1_cache_references;
    shm->timing.guest_perf.llc_misses = timing.perf_results.llc_misses;
    shm->timing.guest_perf.llc_references = timing.perf_results.llc_references;
    shm->timing.guest_perf.memory_loads = timing.perf_results.memory_loads;
    shm->timing.guest_perf.memory_stores = timing.perf_results.memory_stores;
    shm->timing.guest_perf.tlb_misses = timing.perf_results.tlb_misses;
    shm->timing.guest_perf.cpu_cycles = timing.perf_results.cpu_cycles;
    shm->timing.guest_perf.instructions = timing.perf_results.instructions;
    shm->timing.guest_perf.context_switches = timing.perf_results.context_switches;
    
    // Convert rates to fixed-point integers (multiply by 10000)
    shm->timing.guest_perf.l1_cache_miss_rate_x10000 = (uint32_t)(timing.perf_results.l1_cache_miss_rate * 10000.0);
    shm->timing.guest_perf.llc_cache_miss_rate_x10000 = (uint32_t)(timing.perf_results.llc_cache_miss_rate * 10000.0);
    shm->timing.guest_perf.instructions_per_cycle_x10000 = (uint32_t)(timing.perf_results.instructions_per_cycle * 10000.0);
    shm->timing.guest_perf.cycles_per_byte_x10000 = (uint32_t)(timing.perf_results.cycles_per_byte * 10000.0);
    shm->timing.guest_perf.tlb_miss_rate_x10000 = (uint32_t)(timing.perf_results.tlb_miss_rate * 10000.0);
    
    __sync_synchronize();
    
    // Display results
    display_performance_results(data_size, &timing, perf_available, use_write_combining, hash_match);
    
    if (!success) {
        shm->error_code = error_code;
        __sync_synchronize();
    }
    
    // Cleanup measurement buffer
    free(measurement_buffer);
    
    return success;
}

void monitor_latency(volatile struct shared_data *shm, bool expect_latency, bool expect_bandwidth, int expected_count, bool use_write_combining)
{
    printf("Guest Reader - Monitoring for messages from host...\n");
    printf("Expected: %s%s%s (count: %d)\n", 
           expect_latency ? "latency " : "",
           (expect_latency && expect_bandwidth) ? "+ " : "",
           expect_bandwidth ? "bandwidth" : "",
           expected_count);
    printf("Will measure: 64-bit read+XOR vs memcpy performance comparison (all data accessed)\n");
    printf("Plus SHA256 verification time (testing only, not real overhead)\n");
    printf("Memory access mode: %s\n", 
           use_write_combining ? "Write-combining (may improve write performance)" : "Standard cached access");
    printf("\n");
    fflush(stdout);
    
    // STATE: GUEST_STATE_UNINITIALIZED -> GUEST_STATE_WAITING_HOST_INIT
    set_guest_state(shm, GUEST_STATE_WAITING_HOST_INIT);
    
    // Initialize guest communication and wait for host handshake
    if (!initialize_guest_communication(shm)) {
        exit(1);
    }
    
    // STATE: GUEST_STATE_WAITING_HOST_INIT -> GUEST_STATE_READY
    set_guest_state(shm, GUEST_STATE_READY);
    
    // Allocate local buffer for all messages (max 4K frame size)
    size_t max_buffer_size = 3840 * 2160 * 3;
    uint8_t *local_buffer = malloc(max_buffer_size);
    if (!local_buffer) {
        printf("GUEST: ERROR - Failed to allocate local buffer\n");
        exit(1);
    }
    
    // Setup performance counters
    struct perf_counters perf_counters;
    bool perf_available = setup_performance_counters(&perf_counters);
    
    // Main message processing loop
    int message_count = 0;
    while (message_count < expected_count) {
        if (shm->test_complete == 1) {
            printf("Test completion signal received. Exiting...\n");
            break;
        }
        
        // Wait for host to start sending (HOST_STATE_SENDING)
        while (get_host_state(shm) != HOST_STATE_SENDING && shm->test_complete == 0) {
            usleep(10);
        }
        
        if (shm->test_complete == 1) {
            printf("Test completion signal received during wait. Exiting...\n");
            break;
        }
        
        message_count++;
        printf("\n=== Message %d Received ===\n", message_count);
        
        // Process the message
        bool success = process_single_message(shm, local_buffer, perf_available, &perf_counters, use_write_combining);
        
        // STATE: GUEST_STATE_PROCESSING -> GUEST_STATE_ACKNOWLEDGED
        set_guest_state(shm, GUEST_STATE_ACKNOWLEDGED);
        
        // Wait for host to finish with this message
        while (get_host_state(shm) != HOST_STATE_READY && shm->test_complete == 0) {
            usleep(10);
        }
        
        // STATE: GUEST_STATE_ACKNOWLEDGED -> GUEST_STATE_READY
        set_guest_state(shm, GUEST_STATE_READY);
        
        // Continue even if message processing failed (for debugging)
        (void)success;
    }
    
    printf("Guest monitoring loop ended after %d messages\n", message_count);
    
    // Cleanup
    free(local_buffer);
    
    if (perf_available) {
        perf_counters_cleanup(&perf_counters);
    }
}

int main(int argc, char *argv[])
{
    printf("Guest Reader - ivshmem Performance Test with Timing Analysis\n");
    printf("============================================================\n\n");
    
    // Parse command line arguments
    bool expect_latency = false;
    bool expect_bandwidth = false;
    bool use_write_combining = false;
    int latency_count = 1000;
    int bandwidth_count = 10;
    int custom_count = -1;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--latency") == 0) {
            expect_latency = true;
            if (i + 1 < argc && isdigit(argv[i + 1][0])) {
                latency_count = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bandwidth") == 0) {
            expect_bandwidth = true;
            if (i + 1 < argc && isdigit(argv[i + 1][0])) {
                bandwidth_count = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--count") == 0) {
            if (i + 1 < argc) {
                custom_count = atoi(argv[++i]);
            } else {
                fprintf(stderr, "Error: -c requires a count argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--write-combining") == 0) {
            use_write_combining = true;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (!expect_latency && !expect_bandwidth) {
        expect_latency = true;
        expect_bandwidth = true;
    }
    
    int expected_count;
    if (custom_count > 0) {
        expected_count = custom_count;
    } else if (expect_latency && expect_bandwidth) {
        expected_count = latency_count + bandwidth_count;
    } else if (expect_latency) {
        expected_count = latency_count;
    } else {
        expected_count = bandwidth_count;
    }
    
    printf("Configuration:\n");
    printf("  Expect latency: %s (%d messages)\n", expect_latency ? "yes" : "no", latency_count);
    printf("  Expect bandwidth: %s (%d iterations)\n", expect_bandwidth ? "yes" : "no", bandwidth_count);
    printf("  Write-combining: %s\n", use_write_combining ? "enabled (resource2_wc)" : "disabled (resource2)");
    printf("  Total expected messages: %d\n\n", expected_count);
    fflush(stdout);
    
    // Check device
    int fd;
    struct stat st;
    const char *device_path;
    
    // Select device path based on write-combining preference
    if (use_write_combining) {
        device_path = PCI_RESOURCE_WC_PATH;
        if (access(PCI_RESOURCE_WC_PATH, F_OK) != 0) {
            printf("ERROR: Write-combining resource2_wc not available\n");
            printf("Falling back to regular resource2...\n");
            device_path = PCI_RESOURCE_PATH;
            use_write_combining = false;
        }
    } else {
        device_path = PCI_RESOURCE_PATH;
    }
    
    if (access(device_path, F_OK) != 0) {
        printf("INFO: PCI device not found, trying shared memory for host testing...\n");
        device_path = SHMEM_PATH;
        use_write_combining = false;  // Shared memory doesn't support write-combining
        
        if (access(SHMEM_PATH, F_OK) != 0) {
            printf("ERROR: Neither PCI device nor shared memory found\n");
            printf("Make sure you're running this inside the VM or have shared memory set up.\n");
            return 1;
        }
    }
    
    // Open resource
    fd = open(device_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Failed to open device resource");
        printf("\nTry running with sudo:\n");
        printf("  sudo %s\n", argv[0]);
        return 1;
    }
    
    // Get resource size
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    
    printf("Resource: %s %s\n", device_path, 
           use_write_combining ? "(write-combining enabled)" : "(standard caching)");
    printf("Resource size: %ld bytes (%ld MB)\n", 
           st.st_size, st.st_size / (1024 * 1024));
    fflush(stdout);
    
    // Map memory
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
    
    // Initialize guest state
    set_guest_state(shm, GUEST_STATE_UNINITIALIZED);
    
    printf("Mapped at address: %p\n", ptr);
    printf("Ready to receive data from host.\n\n");
    fflush(stdout);
    
    printf("Initial values:\n");
    printf("  Magic: 0x%08X\n", shm->magic);
    printf("  Sequence: %u\n", shm->sequence);
    printf("  Data size: %u\n", shm->data_size);
    printf("  Error code: %u\n", shm->error_code);
    printf("  Test complete: %u\n", shm->test_complete);
    printf("  Host state: %u (%s)\n", shm->host_state, host_state_name((host_state_t)shm->host_state));
    printf("  Guest state: %u (%s)\n", shm->guest_state, guest_state_name((guest_state_t)shm->guest_state));
    printf("\n");
    fflush(stdout);
    
    // Start monitoring
    monitor_latency(shm, expect_latency, expect_bandwidth, expected_count, use_write_combining);
    
    // Cleanup
    munmap(ptr, st.st_size);
    close(fd);
    
    return 0;
}