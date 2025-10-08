/*
 * common.h - Shared definitions for ivshmem host-guest communication
 * 
 * This header contains all shared data structures, enums, and constants
 * used by both host_writer.c and guest_reader.c
 */

#ifndef IVSHMEM_COMMON_H
#define IVSHMEM_COMMON_H

#include <stdint.h>

#define MAGIC 0xDEADBEEF

// Host state machine states - only modified by host
typedef enum {
    HOST_STATE_UNINITIALIZED = 0,
    HOST_STATE_INITIALIZING = 1,
    HOST_STATE_READY = 2,
    HOST_STATE_SENDING = 3,
    HOST_STATE_COMPLETED = 4
} host_state_t;

// Guest state machine states - only modified by guest  
typedef enum {
    GUEST_STATE_UNINITIALIZED = 0,
    GUEST_STATE_WAITING_HOST_INIT = 1,
    GUEST_STATE_READY = 2,
    GUEST_STATE_PROCESSING = 3,
    GUEST_STATE_ACKNOWLEDGED = 4
} guest_state_t;

// Timing measurements structure for detailed overhead analysis
// IMPORTANT: Host and guest clocks are NOT synchronized!
// Guest measures durations and reports them; host measures its own durations.
// Never compare absolute timestamps across host/guest boundary.
struct timing_data {
    // Guest-side DURATIONS (nanoseconds) - measured on guest clock
    uint64_t guest_copy_duration;    // Time to memcpy from shared memory to local buffer
    uint64_t guest_verify_duration;  // Time to compute SHA256 verification (testing only)
    uint64_t guest_total_duration;   // Total processing time on guest
    
    // Reserved for future use
    uint64_t reserved[5];
};

// Shared memory layout for cross-VM communication
struct shared_data {
    // Initialization and termination control
    uint32_t magic;           // Magic number to verify sync (0 = initializing, MAGIC = ready)
    uint32_t test_complete;   // 1 to signal test completion
    
    // State machine tracking (each side only modifies their own state)
    uint32_t host_state;      // Current host state (host_state_t) - host writes, guest reads
    uint32_t guest_state;     // Current guest state (guest_state_t) - guest writes, host reads
    
    // Message data
    uint32_t sequence;        // Sequence number
    uint32_t data_size;       // Size of data in buffer
    uint8_t  data_sha256[32]; // SHA256 of the data buffer
    uint32_t error_code;      // Error code if processing failed
    
    // Timing measurements for overhead analysis
    struct timing_data timing;
    
    // Alignment and buffer
    uint8_t  padding[0];      // Let compiler handle alignment
    char     _align[0] __attribute__((aligned(64)));
    
    uint8_t  buffer[0];       // Actual data buffer
};

// State name conversion functions
static inline const char* host_state_name(host_state_t state)
{
    switch (state) {
        case HOST_STATE_UNINITIALIZED: return "UNINITIALIZED";
        case HOST_STATE_INITIALIZING: return "INITIALIZING";  
        case HOST_STATE_READY: return "READY";
        case HOST_STATE_SENDING: return "SENDING";
        case HOST_STATE_COMPLETED: return "COMPLETED";
        default: return "UNKNOWN";
    }
}

static inline const char* guest_state_name(guest_state_t state)
{
    switch (state) {
        case GUEST_STATE_UNINITIALIZED: return "UNINITIALIZED";
        case GUEST_STATE_WAITING_HOST_INIT: return "WAITING_HOST_INIT";
        case GUEST_STATE_READY: return "READY";
        case GUEST_STATE_PROCESSING: return "PROCESSING";
        case GUEST_STATE_ACKNOWLEDGED: return "ACKNOWLEDGED";
        default: return "UNKNOWN";
    }
}

#endif // IVSHMEM_COMMON_H