#include "exp_table.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

namespace exp_table {

exp_entry *g_entries = nullptr;
size_t g_n_entries = 0;
bool g_enabled = true;
bool g_allocated_psram = false;
bool g_inited = false;

static constexpr size_t EXP_BYTES = 1 * 1024 * 1024; // 1 MB

bool init() {
    if (g_entries) return true;
    size_t bytes = EXP_BYTES;
    size_t want_entries = bytes / sizeof(exp_entry);
#if defined(ESP32)
    void *mem = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem) {
        memset(mem, 0, bytes);
        g_entries = reinterpret_cast<exp_entry*>(mem);
        g_n_entries = want_entries;
        g_allocated_psram = true;
        g_inited = true;
        printf("# experience %zu bytes PSRAM (%zu entries, %zu bytes each)\n", bytes, g_n_entries, sizeof(exp_entry));
        return true;
    }
    printf("# experience PSRAM alloc failed (%zu bytes), trying internal\n", bytes);
    mem = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (mem) {
        memset(mem, 0, bytes);
        g_entries = reinterpret_cast<exp_entry*>(mem);
        g_n_entries = want_entries;
        g_allocated_psram = false;
        g_inited = true;
        printf("# experience %zu bytes internal (%zu entries)\n", bytes, g_n_entries);
        return true;
    }
    // try smaller fallback: 512K
    bytes = 512 * 1024;
    want_entries = bytes / sizeof(exp_entry);
    mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (mem) {
        memset(mem, 0, bytes);
        g_entries = reinterpret_cast<exp_entry*>(mem);
        g_n_entries = want_entries;
        g_allocated_psram = true;
        g_inited = true;
        printf("# experience fallback %zu bytes PSRAM (%zu entries)\n", bytes, g_n_entries);
        return true;
    }
    mem = calloc(1, bytes);
    if (mem) {
        g_entries = reinterpret_cast<exp_entry*>(mem);
        g_n_entries = want_entries;
        g_allocated_psram = false;
        g_inited = true;
        printf("# experience fallback %zu bytes calloc\n", bytes);
        return true;
    }
    printf("# experience disabled (allocation failed)\n");
    g_entries = nullptr;
    g_n_entries = 0;
    g_enabled = false;
    return false;
#else
    void *mem = nullptr;
#if defined(linux) || defined(__APPLE__)
    if (posix_memalign(&mem, 16, bytes) != 0) mem = nullptr;
#endif
    if (!mem) mem = calloc(1, bytes);
    if (!mem) {
        printf("# experience disabled (allocation failed)\n");
        g_entries = nullptr;
        g_n_entries = 0;
        g_enabled = false;
        return false;
    }
    memset(mem, 0, bytes);
    g_entries = reinterpret_cast<exp_entry*>(mem);
    g_n_entries = want_entries;
    g_allocated_psram = false;
    g_inited = true;
    printf("# experience %zu bytes desktop (%zu entries)\n", bytes, g_n_entries);
    return true;
#endif
}

void free_table() {
    if (!g_entries) return;
#if defined(ESP32)
    if (g_allocated_psram) heap_caps_free(g_entries);
    else free(g_entries);
#else
    free(g_entries);
#endif
    g_entries = nullptr;
    g_n_entries = 0;
    g_inited = false;
}

void clear() {
    if (!g_entries) return;
    memset(g_entries, 0, g_n_entries * sizeof(exp_entry));
    printf("# experience cleared\n");
}

bool load() {
    if (!g_entries) return false;
    const char *path =
#if defined(ESP32)
        "/spiffs/experience.bin";
#else
        "experience.bin";
#endif
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("# experience no file %s, starting empty\n", path);
        return false;
    }
    // file may be larger/smaller; merge deeper entries
    // read into temp then merge
    size_t file_bytes = 0;
    fseek(f, 0, SEEK_END);
    file_bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t entries_in_file = file_bytes / sizeof(exp_entry);
    size_t to_read = std::min(entries_in_file, g_n_entries);
    // simple: if sizes match, direct read with merge by depth
    // we'll read into tmp and merge
    exp_entry *tmp = reinterpret_cast<exp_entry*>(malloc(to_read * sizeof(exp_entry)));
    if (!tmp) {
        fclose(f);
        return false;
    }
    size_t n = fread(tmp, sizeof(exp_entry), to_read, f);
    fclose(f);
    if (n != to_read) {
        printf("# experience load incomplete %zu/%zu\n", n, to_read);
        free(tmp);
        return false;
    }
    // merge: keep deeper entry per slot (direct mapped index depends on hash, not file position)
    // So we cannot just memcpy; need to re-insert via store logic to respect fastrange indexing.
    // Instead, iterate tmp and insert with depth check.
    size_t merged = 0;
    for (size_t i = 0; i < n; i++) {
        exp_entry &e = tmp[i];
        if (e.hash == 0) continue;
        // compute target index via its hash
        uint32_t idx = fastrange32(e.hash, uint32_t(g_n_entries));
        exp_entry &cur = g_entries[idx];
        if (cur.hash == 0 || cur.depth <= e.depth) {
            cur = e;
            merged++;
        }
    }
    free(tmp);
    printf("# experience loaded %zu entries merged %zu from %s (%zu bytes)\n", n, merged, path, file_bytes);
    return true;
}

bool save() {
    if (!g_entries) return false;
    const char *path =
#if defined(ESP32)
        "/spiffs/experience.bin";
#else
        "experience.bin";
#endif
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("# experience save failed: cannot open %s\n", path);
        return false;
    }
    size_t n = fwrite(g_entries, sizeof(exp_entry), g_n_entries, f);
    fclose(f);
    if (n != g_n_entries) {
        printf("# experience save incomplete %zu/%zu\n", n, g_n_entries);
        return false;
    }
    printf("# experience saved %zu entries to %s\n", n, path);
    return true;
}

} // namespace exp_table
