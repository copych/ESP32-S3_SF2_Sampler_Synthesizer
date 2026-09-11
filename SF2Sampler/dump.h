#pragma once
#include "SF2Parser.h"
#include "esp_heap_caps.h"
#include "esp_log.h"


static inline const char* memRegion(const void* p)
{
    uintptr_t a = (uintptr_t)p;
    if (a >= 0x3D000000 && a < 0x3E000000) return "PSRAM";
    if (a >= 0x3F000000 && a < 0x40000000) return "DRAM";
    return "UNKNOWN";
}

void dumpCachedSamples(SF2Parser& parser)
{
    auto& samples = parser.getSamples();

    uint32_t totalBytes = 0;
    uint32_t cachedCount = 0;

    ESP_LOGI("", "=== SAMPLE CACHE DUMP START ===");

    for (size_t i = 0; i < samples.size(); ++i) {
        auto& s = samples[i];

        if (!s.cached || !s.data) continue;

        cachedCount++;

        uint32_t bytes = s.dataSize;
        totalBytes += bytes;

        ESP_LOGI("",
            "[%03u] %-20s | %8u bytes | %p | %s | blocks=%u | ref=%u",
            (unsigned)i,
            s.name,
            (unsigned)bytes,
            s.data,
            memRegion(s.data),
            s.poolBlocks,
            s.refCount
        );

        // ---- critical checks ----

        if (((uintptr_t)s.data) & 3) {
            ESP_LOGE("", "  !! UNALIGNED (will break Voice)");
        }

        if (s.end <= s.start) {
            ESP_LOGE("", "  !! INVALID RANGE start=%u end=%u", s.start, s.end);
        }

        if (s.dataSize > s.poolBlocks * 4096) {
            ESP_LOGE("", "  !! OVERFLOW dataSize=%u pool=%u",
                     s.dataSize, s.poolBlocks * 4096);
        }

        // PSRAM probe (safe but revealing)
        volatile uint8_t b0 = s.data[0];
        volatile uint8_t b1 = s.data[1];
        (void)b0; (void)b1;
    }

    ESP_LOGI("", "Cached: %u / %u", cachedCount, (unsigned)samples.size());
    ESP_LOGI("", "Total: %u bytes (%.2f MB)",
             totalBytes,
             totalBytes / (1024.0f * 1024.0f));

    ESP_LOGI("", "PSRAM free: %u",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ESP_LOGI("", "=== SAMPLE CACHE DUMP END ===");
}