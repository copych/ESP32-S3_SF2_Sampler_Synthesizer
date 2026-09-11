#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"

// ===== CONFIG (upper bounds only) =====

#define POOL_BLOCK_SIZE   4096u
#define POOL_BLOCK_SHIFT  12u

#define HASH_SIZE              512u   // keep power of 2

// ===== TYPES =====

struct SampleHandle {
    const int16_t* data;
    uint32_t length;

    uint32_t loopStart;
    uint32_t loopEnd;

    uint32_t sampleRate;
    int8_t   rootKey;
};

struct SampleEntry {
    const int16_t* data;
    uint32_t length;

    uint32_t loopStart;
    uint32_t loopEnd;

    uint32_t sampleRate;
    int8_t   rootKey;

    uint32_t key;
    uint32_t offset;

    uint16_t refCount;
    uint8_t  used;
};

// ===== POOL =====

class SamplePool {
public:
    bool init(uint32_t reserveBytes) {
        memset(hashTable, 0, sizeof(hashTable));

        uint32_t free = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        if (free <= reserveBytes + POOL_BLOCK_SIZE)
            return false;

        uint32_t target = free - reserveBytes;

        // align down to block size
        target &= ~(POOL_BLOCK_SIZE - 1);

        maxBlocks = target >> POOL_BLOCK_SHIFT;
        totalBytes = maxBlocks << POOL_BLOCK_SHIFT;

        // --- allocate pool ---
        rawPool = (uint8_t*)heap_caps_malloc(
            totalBytes + 64,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (!rawPool) return false;

        uintptr_t aligned = ((uintptr_t)rawPool + 63u) & ~63u;
        pool = (uint8_t*)aligned;

        // --- allocate bitmap in INTERNAL RAM (important) ---
        blockUsed = (uint8_t*)heap_caps_malloc(
            maxBlocks,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
        if (!blockUsed) {
            heap_caps_free(rawPool);
            rawPool = nullptr;
            pool = nullptr;
            return false;
        }

        memset(blockUsed, 0, maxBlocks);

        ESP_LOGI("POOL",
            "INIT: total=%u KB blocks=%u blockSize=%u",
            totalBytes >> 10, maxBlocks, POOL_BLOCK_SIZE);

        return true;
    }

    // Reset allocations without releasing the arena itself.
    // Used only for rebuilding a fragmented layout of the SAME parsed SF2.
    // No live voice may point into the pool when this is called.
    inline void reset() {
        if (blockUsed && maxBlocks) memset(blockUsed, 0, maxBlocks);
        memset(hashTable, 0, sizeof(hashTable));
    }

    // Release the complete arena back to PSRAM. This is required before
    // parsing/replacing an SF2 so metadata gets first claim on memory.
    inline void deinit() {
        if (blockUsed) {
            heap_caps_free(blockUsed);
            blockUsed = nullptr;
        }
        if (rawPool) {
            heap_caps_free(rawPool);
            rawPool = nullptr;
        }

        pool = nullptr;
        maxBlocks = 0;
        totalBytes = 0;
        memset(hashTable, 0, sizeof(hashTable));
    }

    // Lookup without changing ownership.
    inline SampleHandle* get(uint32_t key) {
        SampleEntry* e = find(key);
        return e ? (SampleHandle*)e : nullptr;
    }

    inline SampleHandle* acquire(uint32_t key) {
        SampleEntry* e = find(key);
        
        if (!e) return nullptr;
        if (e->refCount == UINT16_MAX) {
            ESP_LOGE("POOL", "refCount overflow key=%u", key);
            return nullptr;
        }
        e->refCount++;
        return (SampleHandle*)e;
    }

    inline void release(uint32_t key) {
        SampleEntry* e = find(key);
        if (!e) return;

        if (e->refCount == 0) {
            ESP_LOGE("POOL", "release underflow key=%u", key);
            return;
        }

        if (--e->refCount == 0) {
            freeBlocks(e->offset, e->length << 1);
            e->data = nullptr;
            e->used = 2;
        }
    }

    // Remove an uncommitted entry, e.g. after an SD read failure.
    inline void discard(uint32_t key) {
        SampleEntry* e = find(key);
        if (!e) return;
        freeBlocks(e->offset, e->length << 1);
        e->data = nullptr;
        e->refCount = 0;
        e->used = 2;
    }

    SampleHandle* insert(
        uint32_t key,
        const int16_t* src,
        uint32_t lengthSamples,
        uint32_t loopStart,
        uint32_t loopEnd,
        uint32_t sampleRate,
        int8_t   rootKey
    ) {
        SampleEntry* existing = find(key);
        if (existing) return (SampleHandle*)existing;

        uint32_t sizeBytes = lengthSamples << 1;
        uint32_t blocksNeeded =
            (sizeBytes + (POOL_BLOCK_SIZE - 1)) >> POOL_BLOCK_SHIFT;

        int32_t startBlock = allocBlocks(blocksNeeded);
        if (startBlock < 0) return nullptr;

        uint32_t offset = ((uint32_t)startBlock) << POOL_BLOCK_SHIFT;
        int16_t* dst = (int16_t*)(pool + offset);

        memcpy(dst, src, sizeBytes);

        SampleEntry* e = insertEntry(key); 
        if (!e) {
            freeBlocks(((uint32_t)startBlock) << POOL_BLOCK_SHIFT, sizeBytes);
            return nullptr;
        }

        e->data       = dst;
        e->length     = lengthSamples;
        e->loopStart  = loopStart;
        e->loopEnd    = loopEnd;
        e->sampleRate = sampleRate;
        e->rootKey    = rootKey;

        e->key        = key;
        e->offset     = offset;

        e->refCount   = 0;
        e->used       = 1;

        return (SampleHandle*)e;
    }

    
    int32_t allocBlocks(uint32_t count) {
        uint32_t run = 0;
        uint32_t start = 0;

        for (uint32_t i = 0; i < maxBlocks; i++) {
            uint8_t used = blockUsed[i];

            run = used ? 0 : (run + 1);
            start = (run == 1) ? i : start;

            if (run >= count) {
                for (uint32_t j = 0; j < count; j++)
                    blockUsed[start + j] = 1;

                return (int32_t)start;
            }
        }
        return -1;
    }

    inline SampleHandle* insertEmpty(
        uint32_t key,
        uint32_t lengthSamples,
        uint32_t loopStart,
        uint32_t loopEnd,
        uint32_t sampleRate,
        int8_t   rootKey
    ) {
        // reuse if already present
        SampleEntry* existing = find(key);
        if (existing) return (SampleHandle*)existing;

        uint32_t sizeBytes = lengthSamples << 1;

        uint32_t blocksNeeded =
            (sizeBytes + (POOL_BLOCK_SIZE - 1)) >> POOL_BLOCK_SHIFT;

        int32_t startBlock = allocBlocks(blocksNeeded);
        if (startBlock < 0) return nullptr;

        SampleEntry* e = insertEntry(key);
        if (!e) {
            freeBlocks(((uint32_t)startBlock) << POOL_BLOCK_SHIFT, sizeBytes);
            return nullptr;
        }

        uint32_t offset = ((uint32_t)startBlock) << POOL_BLOCK_SHIFT;

        e->data       = (int16_t*)(pool + offset);
        e->length     = lengthSamples;
        e->loopStart  = loopStart;
        e->loopEnd    = loopEnd;
        e->sampleRate = sampleRate;
        e->rootKey    = rootKey;

        e->key        = key;
        e->offset     = offset;

        e->refCount   = 0;
        e->used       = 1;

        return (SampleHandle*)e;
    }

    inline SampleEntry* insertEntry(uint32_t key) {
        uint32_t idx = hash(key);
        SampleEntry* firstDeleted = nullptr;

        for (uint32_t i = 0; i < HASH_SIZE; i++) {
            SampleEntry& e = hashTable[idx];

            if (e.used == 1) {
                if (e.key == key) {
                    return &e;  // already exists
                }
            } else if (e.used == 2) {
                // remember tombstone but keep probing
                if (!firstDeleted) firstDeleted = &e;
            } else { // used == 0 (empty)
                return firstDeleted ? firstDeleted : &e;
            }

            idx = (idx + 1) & (HASH_SIZE - 1);
        }

        return firstDeleted;
    }


    uint8_t* pool = nullptr;
    
private:
    uint8_t* rawPool = nullptr;

    uint32_t maxBlocks = 0;
    uint32_t totalBytes = 0;
    uint8_t* blockUsed = nullptr;
    SampleEntry hashTable[HASH_SIZE];

    // ===== HASH =====

    inline uint32_t hash(uint32_t k) const {
        return (k * 2654435761u) & (HASH_SIZE - 1);
    }

    inline SampleEntry* find(uint32_t k) {
        uint32_t idx = hash(k);

        for (uint32_t i = 0; i < HASH_SIZE; i++) {
            SampleEntry& e = hashTable[idx];

            if (e.used == 0) return nullptr;     // only true empty stops search
            if (e.used == 1 && e.key == k) return &e;

            idx = (idx + 1) & (HASH_SIZE - 1);
        }
        return nullptr;
    }

    // ===== ALLOC =====


    inline void freeBlocks(uint32_t offset, uint32_t sizeBytes) {
        uint32_t start = offset >> POOL_BLOCK_SHIFT;
        uint32_t count =
            (sizeBytes + (POOL_BLOCK_SIZE - 1)) >> POOL_BLOCK_SHIFT;

        for (uint32_t i = 0; i < count; i++)
            blockUsed[start + i] = 0;
    }
 
};
