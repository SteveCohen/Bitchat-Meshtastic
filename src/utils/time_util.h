#pragma once

#include <cstdint>
#include <sys/time.h>

// Returns Unix epoch in milliseconds using gettimeofday().
// After NTP sync, this gives real wall-clock time.
// Before sync, falls back to millis() (device uptime).
static inline uint64_t bitchat_epoch_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    // If time is before 2020-01-01, NTP hasn't synced yet — use millis()
    if (tv.tv_sec < 1577836800) {
        return (uint64_t)millis();
    }
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

static inline bool time_is_synced() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec >= 1577836800;  // 2020-01-01
}

// Bootstrap wall-clock time from an external Unix timestamp (e.g. Meshtastic rx_time).
// Only applies if the clock hasn't been set yet (no NTP, no prior sync).
// Returns true if the clock was actually updated.
static inline bool time_sync_from_epoch(uint32_t unix_seconds) {
    if (time_is_synced()) return false;          // already have good time
    if (unix_seconds < 1577836800) return false;  // bogus timestamp (before 2020)
    struct timeval tv = { .tv_sec = (time_t)unix_seconds, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    return true;
}
