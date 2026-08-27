#include "corr_hist.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(ESP32)
#include <esp_heap_caps.h>
#include <esp_timer.h>
#else
#include <sys/time.h>
#endif

namespace corr_hist {

int16_t *g_table = nullptr;
bool g_enabled = true;
bool g_allocated_psram = false;
bool g_inited = false;

bool init() {
    if (g_table) return true;
    const size_t bytes = CORR_SIZE * sizeof(int16_t);
#if defined(ESP32)
    void *mem = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem) {
        memset(mem, 0, bytes);
        g_table = reinterpret_cast<int16_t*>(mem);
        g_allocated_psram = true;
        g_inited = true;
        printf("# corrhist %zu bytes PSRAM (16-aligned)\n", bytes);
        return true;
    }
    printf("# corrhist PSRAM alloc failed (%zu bytes), trying internal\n", bytes);
    mem = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (mem) {
        memset(mem, 0, bytes);
        g_table = reinterpret_cast<int16_t*>(mem);
        g_allocated_psram = false;
        g_inited = true;
        printf("# corrhist %zu bytes internal (fallback)\n", bytes);
        return true;
    }
    // last fallback: calloc
    mem = calloc(1, bytes);
    if (mem) {
        g_table = reinterpret_cast<int16_t*>(mem);
        g_allocated_psram = false;
        g_inited = true;
        printf("# corrhist %zu bytes calloc fallback\n", bytes);
        return true;
    }
    printf("# corrhist disabled (allocation failed)\n");
    g_table = nullptr;
    g_enabled = false;
    return false;
#else
    // Desktop: try aligned_alloc / posix_memalign, else malloc
    void *mem = nullptr;
#if defined(linux) || defined(__APPLE__)
    if (posix_memalign(&mem, 16, bytes) != 0) mem = nullptr;
#else
    mem = aligned_alloc(16, bytes);
#endif
    if (!mem) mem = calloc(1, bytes);
    if (!mem) {
        printf("# corrhist disabled (allocation failed)\n");
        g_table = nullptr;
        g_enabled = false;
        return false;
    }
    memset(mem, 0, bytes);
    g_table = reinterpret_cast<int16_t*>(mem);
    g_allocated_psram = false;
    g_inited = true;
    printf("# corrhist %zu bytes desktop (aligned)\n", bytes);
    return true;
#endif
}

void free_table() {
    if (!g_table) return;
#if defined(ESP32)
    if (g_allocated_psram) {
        heap_caps_free(g_table);
    } else {
        free(g_table);
    }
#else
    free(g_table);
#endif
    g_table = nullptr;
    g_inited = false;
}

void clear() {
    if (!g_table) return;
    memset(g_table, 0, CORR_SIZE * sizeof(int16_t));
    printf("# corrhist cleared\n");
}

bool load() {
    if (!g_table) return false;
    const char *path =
#if defined(ESP32)
        "/spiffs/corrhist.bin";
#else
        "corrhist.bin";
#endif
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("# corrhist no file %s, starting empty\n", path);
        return false;
    }
    size_t n = fread(g_table, 1, CORR_SIZE * sizeof(int16_t), f);
    fclose(f);
    if (n != CORR_SIZE * sizeof(int16_t)) {
        printf("# corrhist load incomplete %zu/%zu, zeroing\n", n, CORR_SIZE * sizeof(int16_t));
        memset(g_table, 0, CORR_SIZE * sizeof(int16_t));
        return false;
    }
    printf("# corrhist loaded %zu bytes from %s\n", n, path);
    return true;
}

bool save() {
    if (!g_table) return false;
    const char *path =
#if defined(ESP32)
        "/spiffs/corrhist.bin";
#else
        "corrhist.bin";
#endif
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("# corrhist save failed: cannot open %s\n", path);
        return false;
    }
    size_t n = fwrite(g_table, 1, CORR_SIZE * sizeof(int16_t), f);
    fclose(f);
    if (n != CORR_SIZE * sizeof(int16_t)) {
        printf("# corrhist save incomplete %zu/%zu\n", n, CORR_SIZE * sizeof(int16_t));
        return false;
    }
    printf("# corrhist saved %zu bytes to %s\n", n, path);
    return true;
}

bool verify_and_indexing() {
    // power of two check: CORR_HALF must be pow2
    if ((CORR_HALF & (CORR_HALF - 1)) != 0) return false;
    // AND vs modulo equivalence for random keys
    for (uint64_t k = 0; k < 10000; k++) {
        uint64_t key = k * 2654435761ull;
        size_t via_and = key & CORR_MASK;
        size_t via_mod = key % CORR_HALF;
        if (via_and != via_mod) return false;
    }
    // also verify two halves don't overlap and total size matches
    if (CORR_SIZE != CORR_HALF * 2) return false;
    return true;
}

} // namespace corr_hist
