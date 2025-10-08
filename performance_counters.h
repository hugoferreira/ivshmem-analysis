/*
 * performance_counters.h - Hardware performance counter integration
 * 
 * Provides hardware-level performance monitoring using perf_event_open()
 * to measure cache hits/misses, memory bandwidth, and CPU events
 */

#ifndef PERFORMANCE_COUNTERS_H
#define PERFORMANCE_COUNTERS_H

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>

// Performance counter file descriptors
struct perf_counters {
    int l1_cache_misses_fd;
    int l1_cache_references_fd;
    int llc_misses_fd;          // Last Level Cache (L3)
    int llc_references_fd;
    int memory_loads_fd;
    int memory_stores_fd;
    int tlb_misses_fd;
    int cpu_cycles_fd;
    int instructions_fd;
    int context_switches_fd;
    bool initialized;
};

// Performance measurement results
struct perf_results {
    uint64_t l1_cache_misses;
    uint64_t l1_cache_references;
    uint64_t llc_misses;
    uint64_t llc_references;
    uint64_t memory_loads;
    uint64_t memory_stores;
    uint64_t tlb_misses;
    uint64_t cpu_cycles;
    uint64_t instructions;
    uint64_t context_switches;
    
    // Calculated metrics
    double l1_cache_miss_rate;      // L1 misses / L1 references
    double llc_cache_miss_rate;     // LLC misses / LLC references
    double instructions_per_cycle;  // IPC
    double cycles_per_byte;         // CPU efficiency for data size
    double tlb_miss_rate;          // TLB misses per memory operation
};

// Initialize performance counters
static inline long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                                  int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// Initialize all performance counters
static bool perf_counters_init(struct perf_counters *counters)
{
    struct perf_event_attr attr;
    memset(counters, 0, sizeof(*counters));
    
    // Initialize all file descriptors to -1
    counters->l1_cache_misses_fd = -1;
    counters->l1_cache_references_fd = -1;
    counters->llc_misses_fd = -1;
    counters->llc_references_fd = -1;
    counters->memory_loads_fd = -1;
    counters->memory_stores_fd = -1;
    counters->tlb_misses_fd = -1;
    counters->cpu_cycles_fd = -1;
    counters->instructions_fd = -1;
    counters->context_switches_fd = -1;
    
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.disabled = 1;
    attr.exclude_kernel = 0;  // Include kernel events for complete picture
    attr.exclude_hv = 1;      // Exclude hypervisor
    
    // L1 Data Cache Misses
    attr.type = PERF_TYPE_HW_CACHE;
    attr.config = PERF_COUNT_HW_CACHE_L1D | 
                  (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                  (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    counters->l1_cache_misses_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    // L1 Data Cache References  
    attr.config = PERF_COUNT_HW_CACHE_L1D |
                  (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                  (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
    counters->l1_cache_references_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    // Last Level Cache (L3) Misses
    attr.config = PERF_COUNT_HW_CACHE_LL |
                  (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                  (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    counters->llc_misses_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    // Last Level Cache References
    attr.config = PERF_COUNT_HW_CACHE_LL |
                  (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                  (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
    counters->llc_references_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    // TLB Misses
    attr.config = PERF_COUNT_HW_CACHE_DTLB |
                  (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                  (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    counters->tlb_misses_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    // CPU Cycles
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = PERF_COUNT_HW_CPU_CYCLES;
    counters->cpu_cycles_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    // Instructions
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    counters->instructions_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    // Context Switches
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_CONTEXT_SWITCHES;
    counters->context_switches_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    // Note: Memory loads/stores may not be available on all architectures
    // Try to open them but don't fail if they're not supported
    attr.type = PERF_TYPE_HW_CACHE;
    attr.config = PERF_COUNT_HW_CACHE_L1D |
                  (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                  (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
    counters->memory_loads_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    attr.config = PERF_COUNT_HW_CACHE_L1D |
                  (PERF_COUNT_HW_CACHE_OP_WRITE << 8) |
                  (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
    counters->memory_stores_fd = perf_event_open(&attr, 0, -1, -1, 0);
    
    // Check if essential counters opened successfully
    if (counters->l1_cache_misses_fd < 0 || counters->cpu_cycles_fd < 0 || 
        counters->instructions_fd < 0) {
        return false;
    }
    
    counters->initialized = true;
    return true;
}

// Start performance measurement
static void perf_counters_start(struct perf_counters *counters)
{
    if (!counters->initialized) return;
    
    // Reset and enable all counters
    if (counters->l1_cache_misses_fd >= 0) {
        ioctl(counters->l1_cache_misses_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->l1_cache_misses_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (counters->l1_cache_references_fd >= 0) {
        ioctl(counters->l1_cache_references_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->l1_cache_references_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (counters->llc_misses_fd >= 0) {
        ioctl(counters->llc_misses_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->llc_misses_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (counters->llc_references_fd >= 0) {
        ioctl(counters->llc_references_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->llc_references_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (counters->tlb_misses_fd >= 0) {
        ioctl(counters->tlb_misses_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->tlb_misses_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (counters->cpu_cycles_fd >= 0) {
        ioctl(counters->cpu_cycles_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->cpu_cycles_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (counters->instructions_fd >= 0) {
        ioctl(counters->instructions_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->instructions_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (counters->context_switches_fd >= 0) {
        ioctl(counters->context_switches_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->context_switches_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (counters->memory_loads_fd >= 0) {
        ioctl(counters->memory_loads_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->memory_loads_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (counters->memory_stores_fd >= 0) {
        ioctl(counters->memory_stores_fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(counters->memory_stores_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
}

// Stop measurement and collect results
static void perf_counters_stop(struct perf_counters *counters, struct perf_results *results, size_t data_size)
{
    if (!counters->initialized) {
        memset(results, 0, sizeof(*results));
        return;
    }
    
    // Disable all counters first
    if (counters->l1_cache_misses_fd >= 0)
        ioctl(counters->l1_cache_misses_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (counters->l1_cache_references_fd >= 0)
        ioctl(counters->l1_cache_references_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (counters->llc_misses_fd >= 0)
        ioctl(counters->llc_misses_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (counters->llc_references_fd >= 0)
        ioctl(counters->llc_references_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (counters->tlb_misses_fd >= 0)
        ioctl(counters->tlb_misses_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (counters->cpu_cycles_fd >= 0)
        ioctl(counters->cpu_cycles_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (counters->instructions_fd >= 0)
        ioctl(counters->instructions_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (counters->context_switches_fd >= 0)
        ioctl(counters->context_switches_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (counters->memory_loads_fd >= 0)
        ioctl(counters->memory_loads_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (counters->memory_stores_fd >= 0)
        ioctl(counters->memory_stores_fd, PERF_EVENT_IOC_DISABLE, 0);
    
    // Read all counter values
    memset(results, 0, sizeof(*results));
    
    if (counters->l1_cache_misses_fd >= 0)
        read(counters->l1_cache_misses_fd, &results->l1_cache_misses, sizeof(uint64_t));
    if (counters->l1_cache_references_fd >= 0)
        read(counters->l1_cache_references_fd, &results->l1_cache_references, sizeof(uint64_t));
    if (counters->llc_misses_fd >= 0)
        read(counters->llc_misses_fd, &results->llc_misses, sizeof(uint64_t));
    if (counters->llc_references_fd >= 0)
        read(counters->llc_references_fd, &results->llc_references, sizeof(uint64_t));
    if (counters->tlb_misses_fd >= 0)
        read(counters->tlb_misses_fd, &results->tlb_misses, sizeof(uint64_t));
    if (counters->cpu_cycles_fd >= 0)
        read(counters->cpu_cycles_fd, &results->cpu_cycles, sizeof(uint64_t));
    if (counters->instructions_fd >= 0)
        read(counters->instructions_fd, &results->instructions, sizeof(uint64_t));
    if (counters->context_switches_fd >= 0)
        read(counters->context_switches_fd, &results->context_switches, sizeof(uint64_t));
    if (counters->memory_loads_fd >= 0)
        read(counters->memory_loads_fd, &results->memory_loads, sizeof(uint64_t));
    if (counters->memory_stores_fd >= 0)
        read(counters->memory_stores_fd, &results->memory_stores, sizeof(uint64_t));
    
    // Calculate derived metrics
    results->l1_cache_miss_rate = results->l1_cache_references > 0 ?
        (double)results->l1_cache_misses / results->l1_cache_references : 0.0;
    
    results->llc_cache_miss_rate = results->llc_references > 0 ?
        (double)results->llc_misses / results->llc_references : 0.0;
    
    results->instructions_per_cycle = results->cpu_cycles > 0 ?
        (double)results->instructions / results->cpu_cycles : 0.0;
        
    results->cycles_per_byte = data_size > 0 && results->cpu_cycles > 0 ?
        (double)results->cpu_cycles / data_size : 0.0;
        
    uint64_t total_memory_ops = results->memory_loads + results->memory_stores;
    results->tlb_miss_rate = total_memory_ops > 0 ?
        (double)results->tlb_misses / total_memory_ops : 0.0;
}

// Cleanup performance counters
static void perf_counters_cleanup(struct perf_counters *counters)
{
    if (!counters->initialized) return;
    
    if (counters->l1_cache_misses_fd >= 0) close(counters->l1_cache_misses_fd);
    if (counters->l1_cache_references_fd >= 0) close(counters->l1_cache_references_fd);
    if (counters->llc_misses_fd >= 0) close(counters->llc_misses_fd);
    if (counters->llc_references_fd >= 0) close(counters->llc_references_fd);
    if (counters->tlb_misses_fd >= 0) close(counters->tlb_misses_fd);
    if (counters->cpu_cycles_fd >= 0) close(counters->cpu_cycles_fd);
    if (counters->instructions_fd >= 0) close(counters->instructions_fd);
    if (counters->context_switches_fd >= 0) close(counters->context_switches_fd);
    if (counters->memory_loads_fd >= 0) close(counters->memory_loads_fd);
    if (counters->memory_stores_fd >= 0) close(counters->memory_stores_fd);
    
    memset(counters, 0, sizeof(*counters));
}

// Print performance results (for debugging)
static void perf_print_results(const struct perf_results *results, const char *operation, size_t data_size)
{
    printf("Performance metrics for %s (%.2f MB):\n", operation, data_size / (1024.0 * 1024.0));
    printf("  L1 Cache: %lu misses / %lu refs (%.1f%% miss rate)\n",
           results->l1_cache_misses, results->l1_cache_references, 
           results->l1_cache_miss_rate * 100.0);
    printf("  LLC Cache: %lu misses / %lu refs (%.1f%% miss rate)\n",
           results->llc_misses, results->llc_references,
           results->llc_cache_miss_rate * 100.0);
    printf("  TLB: %lu misses (%.3f%% miss rate)\n",
           results->tlb_misses, results->tlb_miss_rate * 100.0);
    printf("  CPU: %lu cycles, %lu instructions (IPC: %.2f)\n",
           results->cpu_cycles, results->instructions, results->instructions_per_cycle);
    printf("  Efficiency: %.1f cycles/byte\n", results->cycles_per_byte);
    printf("  Context switches: %lu\n", results->context_switches);
}

#endif // PERFORMANCE_COUNTERS_H
