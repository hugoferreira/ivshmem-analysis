/*
 * guest_reader.c - Guest program to read from ivshmem PCI device
 * 
 * This program reads data from the ivshmem PCI BAR2 and measures performance
 * with detailed timing breakdown of all overheads
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

#define PCI_RESOURCE_PATH "/sys/bus/pci/devices/0000:00:03.0/resource2"
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

// Force read of buffer
static void force_buffer_read(const uint8_t *data, uint32_t size)
{
    volatile uint8_t dummy = 0;
    for (size_t i = 0; i < size; i += 64) {
        dummy ^= data[i];
    }
    (void)dummy;
}

// Print usage
static void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Options:\n");
    printf("  -l, --latency [COUNT]     Expect latency test (default: 100 messages)\n");
    printf("  -b, --bandwidth [COUNT]   Expect bandwidth test (default: 10 iterations)\n");
    printf("  -c, --count COUNT         Number of messages/iterations to expect\n");
    printf("  -h, --help               Show this help\n");
    printf("\n");
}

void monitor_latency(volatile struct shared_data *shm, bool expect_latency, bool expect_bandwidth, int expected_count)
{
    printf("Guest Reader - Monitoring for messages from host...\n");
    printf("Expected: %s%s%s (count: %d)\n", 
           expect_latency ? "latency " : "",
           (expect_latency && expect_bandwidth) ? "+ " : "",
           expect_bandwidth ? "bandwidth" : "",
           expected_count);
    printf("Will measure and report: Read time + Verify time (on guest clock)\n\n");
    fflush(stdout);
    
    int message_count = 0;
    
    // STATE: GUEST_STATE_UNINITIALIZED -> GUEST_STATE_WAITING_HOST_INIT
    set_guest_state(shm, GUEST_STATE_WAITING_HOST_INIT);
    
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
        exit(1);
    }
    
    printf("GUEST: ✓ Host initialization complete - ready for messages.\n\n");
    
    // STATE: GUEST_STATE_WAITING_HOST_INIT -> GUEST_STATE_READY
    set_guest_state(shm, GUEST_STATE_READY);
    
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
        
        // Start total processing timer (on GUEST clock)
        uint64_t processing_start = get_time_ns();
        
        // STATE: GUEST_STATE_READY -> GUEST_STATE_PROCESSING
        set_guest_state(shm, GUEST_STATE_PROCESSING);
        
        // Read message metadata
        uint32_t sequence = shm->sequence;
        uint32_t data_size = shm->data_size;
        
        uint8_t expected_hash[32];
        memcpy(expected_hash, (const void *)shm->data_sha256, 32);
        
        message_count++;
        
        printf("\n=== Message %d Received ===\n", message_count);
        printf("Sequence: %u, Data size: %u bytes (%.2f MB)\n", 
               sequence, data_size, data_size / (1024.0 * 1024.0));
        fflush(stdout);
        
        bool success = true;
        uint32_t error_code = 0;
        
        // Get data pointer
        uint8_t *data_ptr = (uint8_t *)&shm->buffer[0];
        
        // MEASUREMENT 1: Read time (cache flush + buffer read) - on GUEST clock
        uint64_t read_start = get_time_ns();
        
        // Flush cache to ensure we read from actual memory
        flush_cache_range(data_ptr, data_size);
        
        // Force read of entire buffer
        force_buffer_read(data_ptr, data_size);
        
        uint64_t read_end = get_time_ns();
        uint64_t read_duration = read_end - read_start;
        
        // MEASUREMENT 2: Verification time (SHA256) - on GUEST clock
        uint64_t verify_start = get_time_ns();
        
        bool hash_match = verify_data_integrity(data_ptr, data_size, expected_hash);
        
        uint64_t verify_end = get_time_ns();
        uint64_t verify_duration = verify_end - verify_start;
        
        // Calculate total processing duration
        uint64_t processing_end = get_time_ns();
        uint64_t total_duration = processing_end - processing_start;
        
        // WRITE DURATIONS to shared memory for host to read
        shm->timing.guest_read_duration = read_duration;
        shm->timing.guest_verify_duration = verify_duration;
        shm->timing.guest_total_duration = total_duration;
        __sync_synchronize();
        
        // Display results
        printf("Guest Timing (measured on guest clock):\n");
        printf("  Read:         %lu ns (%.2f µs) [%.0f MB/s]\n", 
               read_duration, read_duration / 1000.0, 
               (data_size / (1024.0 * 1024.0)) / (read_duration / 1e9));
        printf("  Verify:       %lu ns (%.2f µs)\n", 
               verify_duration, verify_duration / 1000.0);
        printf("  Total:        %lu ns (%.2f µs)\n", 
               total_duration, total_duration / 1000.0);
        
        if (hash_match) {
            printf("✓ Data integrity verified: SHA256 match\n");
        } else {
            printf("✗ Data integrity check FAILED: SHA256 mismatch\n");
            uint8_t calculated_hash[32];
            SHA256_CTX ctx;
            SHA256_Init(&ctx);
            SHA256_Update(&ctx, data_ptr, data_size);
            SHA256_Final(calculated_hash, &ctx);
            print_hash_comparison(expected_hash, calculated_hash);
            success = false;
            error_code = 1;
        }
        
        printf("  Processing complete\n\n");
        
        if (!success) {
            shm->error_code = error_code;
            __sync_synchronize();
        }
        
        // STATE: GUEST_STATE_PROCESSING -> GUEST_STATE_ACKNOWLEDGED
        set_guest_state(shm, GUEST_STATE_ACKNOWLEDGED);
        
        // Wait for host to finish with this message
        while (get_host_state(shm) != HOST_STATE_READY && shm->test_complete == 0) {
            usleep(10);
        }
        
        // STATE: GUEST_STATE_ACKNOWLEDGED -> GUEST_STATE_READY
        set_guest_state(shm, GUEST_STATE_READY);
    }
    
    printf("Guest monitoring loop ended after %d messages\n", message_count);
}

int main(int argc, char *argv[])
{
    printf("Guest Reader - ivshmem Performance Test with Timing Analysis\n");
    printf("============================================================\n\n");
    
    // Parse command line arguments
    bool expect_latency = false;
    bool expect_bandwidth = false;
    int latency_count = 100;
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
    printf("  Total expected messages: %d\n\n", expected_count);
    fflush(stdout);
    
    // Check device
    int fd;
    struct stat st;
    const char *device_path = PCI_RESOURCE_PATH;
    
    if (access(PCI_RESOURCE_PATH, F_OK) != 0) {
        printf("INFO: PCI device not found, trying shared memory for host testing...\n");
        device_path = SHMEM_PATH;
        
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
    
    printf("Resource: %s\n", device_path);
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
    monitor_latency(shm, expect_latency, expect_bandwidth, expected_count);
    
    // Cleanup
    munmap(ptr, st.st_size);
    close(fd);
    
    return 0;
}