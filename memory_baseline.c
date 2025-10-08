/*
 * memory_baseline.c - Baseline memory performance benchmark
 * 
 * Tests different memory access patterns to establish baseline performance
 * Run this on the HOST to compare against VM performance
 * 
 * SIMD OPTIMIZATION:
 * Includes both standard and SIMD-optimized versions of tests to compare
 * compiler vectorization performance improvements.
 * 
 * Compile (Standard): gcc -O2 -o memory_baseline memory_baseline.c
 * Compile (SIMD):     gcc -O2 -march=native -ftree-vectorize -ffast-math -o memory_baseline memory_baseline.c
 * Run: ./memory_baseline [size_mb] [iterations]
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

// ===== SIMD-OPTIMIZED TEST FUNCTIONS =====

// Test 7: SIMD-optimized 64-bit XOR read (cold cache)
double test_simd_64bit_xor_cold(uint8_t *data, size_t size)
{
    flush_cache_range(data, size);
    
    uint64_t start = get_time_ns();
    
    volatile uint64_t dummy = 0;
    uint64_t * __restrict__ data64 = (uint64_t * __restrict__)data;
    size_t size64 = size / 8;
    size_t remainder = size % 8;
    
    // Hint to compiler for alignment and vectorization
    data64 = (uint64_t *)__builtin_assume_aligned(data64, 8);
    
    // SIMD optimization hints for vectorized 64-bit XOR
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
    
    uint64_t end = get_time_ns();
    
    // Use result to prevent optimization (volatile ensures this isn't optimized away)
    (void)dummy;
    
    return (end - start) / 1e9; // Return seconds
}

// Test 8: SIMD-optimized 64-bit XOR read (hot cache)
double test_simd_64bit_xor_hot(uint8_t *data, size_t size)
{
    // Pre-load into cache
    volatile uint64_t warmup = 0;
    uint64_t *data64_warmup = (uint64_t *)data;
    size_t size64_warmup = size / 8;
    for (size_t i = 0; i < size64_warmup; i += 8) { // Every 8th 64-bit value
        warmup += data64_warmup[i];
    }
    
    uint64_t start = get_time_ns();
    
    volatile uint64_t dummy = 0;
    uint64_t * __restrict__ data64 = (uint64_t * __restrict__)data;
    size_t size64 = size / 8;
    size_t remainder = size % 8;
    
    // Hint to compiler for alignment and vectorization
    data64 = (uint64_t *)__builtin_assume_aligned(data64, 8);
    
    // SIMD optimization hints for vectorized 64-bit XOR (hot cache)
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
    
    uint64_t end = get_time_ns();
    
    // Use result to prevent optimization (volatile ensures this isn't optimized away)
    (void)dummy;
    (void)warmup;
    
    return (end - start) / 1e9; // Return seconds
}

// Test 9: SIMD-optimized byte-by-byte read (cold cache)
double test_simd_byte_by_byte_cold(uint8_t *data, size_t size)
{
    flush_cache_range(data, size);
    
    uint64_t start = get_time_ns();
    
    volatile uint64_t sum = 0;
    uint8_t * __restrict__ data_ptr = (uint8_t * __restrict__)data;
    
    // SIMD optimization hints for vectorized byte addition
    #pragma GCC ivdep
    #pragma clang loop vectorize(enable)
    #pragma GCC unroll 8
    for (size_t i = 0; i < size; i++) {
        sum += data_ptr[i];
    }
    
    uint64_t end = get_time_ns();
    
    // Use result to prevent optimization (volatile ensures this isn't optimized away)
    (void)sum;
    
    return (end - start) / 1e9; // Return seconds
}

// Test 10: SIMD-optimized byte-by-byte read (hot cache)
double test_simd_byte_by_byte_hot(uint8_t *data, size_t size)
{
    // Pre-load into cache
    volatile uint64_t warmup = 0;
    for (size_t i = 0; i < size; i += 64) {
        warmup += data[i];
    }
    
    uint64_t start = get_time_ns();
    
    volatile uint64_t sum = 0;
    uint8_t * __restrict__ data_ptr = (uint8_t * __restrict__)data;
    
    // SIMD optimization hints for vectorized byte addition (hot cache)
    #pragma GCC ivdep
    #pragma clang loop vectorize(enable)
    #pragma GCC unroll 8
    for (size_t i = 0; i < size; i++) {
        sum += data_ptr[i];
    }
    
    uint64_t end = get_time_ns();
    
    // Use result to prevent optimization (volatile ensures this isn't optimized away)
    (void)sum;
    (void)warmup;
    
    return (end - start) / 1e9; // Return seconds
}

// Test 11: SIMD-optimized memory copy (64-bit read+write)
double test_simd_memcpy_64bit(uint8_t *src, uint8_t *dst, size_t size)
{
    flush_cache_range(src, size);
    
    uint64_t start = get_time_ns();
    
    uint64_t * __restrict__ src64 = (uint64_t * __restrict__)src;
    uint64_t * __restrict__ dst64 = (uint64_t * __restrict__)dst;
    size_t size64 = size / 8;
    size_t remainder = size % 8;
    
    // Hint to compiler for alignment and vectorization
    src64 = (uint64_t *)__builtin_assume_aligned(src64, 8);
    dst64 = (uint64_t *)__builtin_assume_aligned(dst64, 8);
    
    // SIMD optimization hints for vectorized 64-bit copy
    #pragma GCC ivdep
    #pragma clang loop vectorize(enable)
    #pragma GCC unroll 4
    for (size_t i = 0; i < size64; i++) {
        dst64[i] = src64[i];
    }
    
    // Handle remainder bytes (non-vectorized)
    for (size_t i = size64 * 8; i < size64 * 8 + remainder; i++) {
        dst[i] = src[i];
    }
    
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
    
    // ===== SIMD-OPTIMIZED TESTS =====
    printf("\n--- SIMD-OPTIMIZED HEAP MEMORY ---\n");
    
    // Test SIMD 64-bit XOR (cold cache) - similar to guest_reader.c optimized loops
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_simd_64bit_xor_cold(heap_src, test_size);
    }
    print_result("SIMD 64bit XOR (cold)", total_time / iterations, test_size, "Vectorized");
    
    // Test SIMD 64-bit XOR (hot cache)
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_simd_64bit_xor_hot(heap_src, test_size);
    }
    print_result("SIMD 64bit XOR (hot)", total_time / iterations, test_size, "From cache");
    
    // Test SIMD byte-by-byte (cold cache)
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_simd_byte_by_byte_cold(heap_src, test_size);
    }
    print_result("SIMD byte-by-byte (cold)", total_time / iterations, test_size, "Vectorized");
    
    // Test SIMD byte-by-byte (hot cache)
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_simd_byte_by_byte_hot(heap_src, test_size);
    }
    print_result("SIMD byte-by-byte (hot)", total_time / iterations, test_size, "From cache");
    
    // Test SIMD memcpy (64-bit read+write)
    total_time = 0;
    for (int i = 0; i < iterations; i++) {
        total_time += test_simd_memcpy_64bit(heap_src, heap_dst, test_size);
    }
    print_result("SIMD memcpy 64bit", total_time / iterations, test_size, "Vectorized");
    
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
        
        // ===== SIMD SHARED MEMORY TESTS =====
        printf("\n--- SIMD-OPTIMIZED SHARED MEMORY ---\n");
        
        // Test SIMD 64-bit XOR from shm (cold)
        total_time = 0;
        for (int i = 0; i < iterations; i++) {
            total_time += test_simd_64bit_xor_cold(shm_ptr, test_size);
        }
        print_result("SIMD 64bit XOR shm (cold)", total_time / iterations, test_size, "Vectorized");
        
        // Test SIMD 64-bit XOR from shm (hot)  
        total_time = 0;
        for (int i = 0; i < iterations; i++) {
            total_time += test_simd_64bit_xor_hot(shm_ptr, test_size);
        }
        print_result("SIMD 64bit XOR shm (hot)", total_time / iterations, test_size, "From cache");
        
        // Test SIMD byte-by-byte from shm (cold)
        total_time = 0;
        for (int i = 0; i < iterations; i++) {
            total_time += test_simd_byte_by_byte_cold(shm_ptr, test_size);
        }
        print_result("SIMD byte shm (cold)", total_time / iterations, test_size, "Vectorized");
        
        // Test SIMD memcpy shm→heap
        total_time = 0;
        for (int i = 0; i < iterations; i++) {
            total_time += test_simd_memcpy_64bit(shm_ptr, heap_dst, test_size);
        }
        print_result("SIMD memcpy shm→heap", total_time / iterations, test_size, "Vectorized");
    }
    
    // Print summary
    printf("\n");
    printf("=== Expected Patterns ===\n");
    printf("Hot cache (L1/L2):      50-100 GB/s\n");
    printf("Cold cache (RAM):       5-20 GB/s\n");
    printf("memcpy optimization:    10-25 GB/s\n");
    printf("Byte-by-byte penalty:   3-5x slower than memcpy\n");
    printf("Stride 64 vs full:      ~60x less data read\n");
    printf("\n");
    printf("=== SIMD Optimization Patterns ===\n");
    printf("SIMD 64bit XOR:         1.5-2x faster than byte-by-byte\n");
    printf("SIMD byte operations:   1.2-1.8x faster than standard\n");
    printf("SIMD memcpy 64bit:      Similar to libc memcpy (already optimized)\n");
    printf("Best SIMD gains:        Computational operations (XOR, ADD)\n");
    printf("Limited SIMD gains:     Memory bandwidth-bound operations\n");
    printf("\n");
    printf("If all tests show similar performance (~GB/s range), memory is healthy.\n");
    printf("If shared memory is much slower (<0.5 GB/s), there's a mapping issue.\n");
    printf("If SIMD shows no improvement, check compiler flags: -march=native -ftree-vectorize\n");
    
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