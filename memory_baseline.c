/*
 * memory_baseline.c - Baseline memory performance benchmark
 * 
 * Tests different memory access patterns to establish baseline performance
 * Run this on the HOST to compare against VM performance
 * 
 * Compile: gcc -O2 -o memory_baseline memory_baseline.c
 * Run: ./memory_baseline
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

// Test size - 24MB (same as 4K frame test)
#define TEST_SIZE (3840 * 2160 * 3)

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
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

// Test 1: Read every 64 bytes (cache line stride)
double test_stride_64(uint8_t *data, size_t size)
{
    flush_cache_range(data, size);
    
    uint64_t start = get_time_ns();
    
    volatile uint64_t sum = 0;
    for (size_t i = 0; i < size; i += 64) {
        sum += data[i];
    }
    
    uint64_t end = get_time_ns();
    
    (void)sum; // Prevent optimization
    
    return (end - start) / 1e9; // Return seconds
}

// Test 2: Read every byte
double test_byte_by_byte(uint8_t *data, size_t size)
{
    flush_cache_range(data, size);
    
    uint64_t start = get_time_ns();
    
    volatile uint64_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum += data[i];
    }
    
    uint64_t end = get_time_ns();
    
    (void)sum; // Prevent optimization
    
    return (end - start) / 1e9; // Return seconds
}

// Test 3: memcpy (cold cache)
double test_memcpy_cold(uint8_t *src, uint8_t *dst, size_t size)
{
    flush_cache_range(src, size);
    
    uint64_t start = get_time_ns();
    
    memcpy(dst, src, size);
    __sync_synchronize();
    
    uint64_t end = get_time_ns();
    
    return (end - start) / 1e9; // Return seconds
}

// Test 4: memcpy (hot cache)
double test_memcpy_hot(uint8_t *src, uint8_t *dst, size_t size)
{
    // Touch source to load into cache
    volatile uint64_t sum = 0;
    for (size_t i = 0; i < size; i += 64) {
        sum += src[i];
    }
    
    uint64_t start = get_time_ns();
    
    memcpy(dst, src, size);
    __sync_synchronize();
    
    uint64_t end = get_time_ns();
    
    return (end - start) / 1e9; // Return seconds
}

// Test 5: Read byte-by-byte (hot cache)
double test_byte_by_byte_hot(uint8_t *data, size_t size)
{
    // Pre-load into cache
    volatile uint64_t sum = 0;
    for (size_t i = 0; i < size; i += 64) {
        sum += data[i];
    }
    
    uint64_t start = get_time_ns();
    
    sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum += data[i];
    }
    
    uint64_t end = get_time_ns();
    
    (void)sum;
    
    return (end - start) / 1e9; // Return seconds
}

// Test 6: memcpy local-to-local (baseline)
double test_memcpy_local(uint8_t *src, uint8_t *dst, size_t size)
{
    flush_cache_range(src, size);
    
    uint64_t start = get_time_ns();
    
    memcpy(dst, src, size);
    __sync_synchronize();
    
    uint64_t end = get_time_ns();
    
    return (end - start) / 1e9; // Return seconds
}

void print_result(const char *test_name, double time_sec, size_t size_bytes, const char *notes)
{
    double size_mb = size_bytes / (1024.0 * 1024.0);
    double bandwidth_mbps = size_mb / time_sec;
    double bandwidth_gbps = bandwidth_mbps / 1024.0;
    
    printf("%-30s %8.2f ms  %8.2f MB/s  %6.2f GB/s  %s\n",
           test_name, time_sec * 1000.0, bandwidth_mbps, bandwidth_gbps, notes);
}

int main(int argc, char *argv[])
{
    size_t test_size = TEST_SIZE;
    int iterations = 10;
    
    // Parse arguments
    if (argc > 1) {
        test_size = atoi(argv[1]) * 1024 * 1024; // MB
    }
    if (argc > 2) {
        iterations = atoi(argv[2]);
    }
    
    printf("Memory Baseline Performance Test\n");
    printf("=================================\n");
    printf("Test size: %.2f MB (%zu bytes)\n", test_size / (1024.0 * 1024.0), test_size);
    printf("Iterations: %d\n\n", iterations);
    
    // Allocate test buffers (heap memory)
    printf("Allocating test buffers in heap (malloc)...\n");
    uint8_t *heap_src = malloc(test_size);
    uint8_t *heap_dst = malloc(test_size);
    
    if (!heap_src || !heap_dst) {
        fprintf(stderr, "Failed to allocate heap buffers\n");
        return 1;
    }
    
    // Initialize with random data
    for (size_t i = 0; i < test_size; i++) {
        heap_src[i] = rand() & 0xFF;
    }
    
    // Test with /dev/shm (shared memory)
    printf("Creating shared memory buffer in /dev/shm...\n");
    int shm_fd = shm_open("/memory_baseline_test", O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        printf("Continuing without shared memory tests...\n");
        shm_fd = -1;
    }
    
    uint8_t *shm_ptr = NULL;
    if (shm_fd >= 0) {
        if (ftruncate(shm_fd, test_size) < 0) {
            perror("ftruncate");
            close(shm_fd);
            shm_fd = -1;
        } else {
            shm_ptr = mmap(NULL, test_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
            if (shm_ptr == MAP_FAILED) {
                perror("mmap");
                close(shm_fd);
                shm_fd = -1;
                shm_ptr = NULL;
            } else {
                memcpy(shm_ptr, heap_src, test_size);
            }
        }
    }
    
    printf("\n");
    printf("Running tests (%d iterations each, showing average)...\n\n", iterations);
    printf("%-30s %10s  %14s  %12s  %s\n", "Test", "Time", "Bandwidth", "Bandwidth", "Notes");
    printf("%-30s %10s  %14s  %12s  %s\n", "----", "----", "---------", "---------", "-----");
    
    double total_time;
    
    // ===== HEAP MEMORY TESTS =====
    printf("\n--- HEAP MEMORY (malloc) ---\n");
    
    // Test 1: Stride 64 (cold cache)
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_stride_64(heap_src, test_size);
    }
    print_result("Stride 64 (cold)", total_time / iterations, test_size / 64, "1/64th data");
    
    // Test 2: Byte-by-byte (cold cache)
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_byte_by_byte(heap_src, test_size);
    }
    print_result("Byte-by-byte (cold)", total_time / iterations, test_size, "Full data");
    
    // Test 3: Byte-by-byte (hot cache)
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_byte_by_byte_hot(heap_src, test_size);
    }
    print_result("Byte-by-byte (hot)", total_time / iterations, test_size, "From cache");
    
    // Test 4: memcpy heap→heap (cold)
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_memcpy_local(heap_src, heap_dst, test_size);
    }
    print_result("memcpy local (cold)", total_time / iterations, test_size, "Optimized");
    
    // Test 5: memcpy heap→heap (hot)
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_memcpy_hot(heap_src, heap_dst, test_size);
    }
    print_result("memcpy local (hot)", total_time / iterations, test_size, "From cache");
    
    // ===== SHARED MEMORY TESTS =====
    if (shm_ptr) {
        printf("\n--- SHARED MEMORY (/dev/shm) ---\n");
        
        // Test 6: Stride 64 from shm (cold)
        total_time = 0;
        for (int i = 0; i < iterations; i++) {
            total_time += test_stride_64(shm_ptr, test_size);
        }
        print_result("Stride 64 shm (cold)", total_time / iterations, test_size / 64, "1/64th data");
        
        // Test 7: Byte-by-byte from shm (cold)
        total_time = 0;
        for (int i = 0; i < iterations; i++) {
            total_time += test_byte_by_byte(shm_ptr, test_size);
        }
        print_result("Byte-by-byte shm (cold)", total_time / iterations, test_size, "Full data");
        
        // Test 8: memcpy shm→heap (cold)
        total_time = 0;
        for (int i = 0; i < iterations; i++) {
            total_time += test_memcpy_cold(shm_ptr, heap_dst, test_size);
        }
        print_result("memcpy shm→heap (cold)", total_time / iterations, test_size, "Optimized");
        
        // Test 9: memcpy shm→heap (hot)
        total_time = 0;
        for (int i = 0; i < iterations; i++) {
            total_time += test_memcpy_hot(shm_ptr, heap_dst, test_size);
        }
        print_result("memcpy shm→heap (hot)", total_time / iterations, test_size, "From cache");
    }
    
    // Print summary
    printf("\n");
    printf("=== Expected Patterns ===\n");
    printf("Hot cache (L1/L2):     50-100 GB/s\n");
    printf("Cold cache (RAM):      5-20 GB/s\n");
    printf("memcpy optimization:   10-25 GB/s\n");
    printf("Byte-by-byte penalty:  3-5x slower than memcpy\n");
    printf("Stride 64 vs full:     ~60x less data read\n");
    printf("\n");
    printf("If all tests show similar performance (~GB/s range), memory is healthy.\n");
    printf("If shared memory is much slower (<0.5 GB/s), there's a mapping issue.\n");
    
    // Cleanup
    free(heap_src);
    free(heap_dst);
    
    if (shm_ptr) {
        munmap(shm_ptr, test_size);
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_unlink("/memory_baseline_test");
    }
    
    return 0;
}