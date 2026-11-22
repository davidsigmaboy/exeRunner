/* pe.c - PE32 (x86) loader: map sections, apply relocations, bind imports to shim thunks. */
#include "er_internal.h"
#include <stdio.h>

static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

#define FAIL(...) do { if (err && err_len) snprintf(err, err_len, __VA_ARGS__); return -1; } while (0)

static int overlaps(uint32_t a, uint32_t asz, uint32_t b, uint32_t bsz) {
    return (uint64_t)a < (uint64_t)b + bsz && (uint64_t)b < (uint64_t)a + asz;
}

static int range_free(Machine *m, uint32_t base, uint32_t size) {
    if (base < 0x00020000u) return 0;
    if ((uint64_t)base + size > m->arena_size) return 0;
    if (overlaps(base, size, HEAP_BASE, HEAP_LIMIT - HEAP_BASE)) return 0;
    if (overlaps(base, size, STACK_TOP - STACK_SIZE - 0x10000u, STACK_SIZE + 0x11000u)) return 0;
    return 1;
}

/* Read a NUL-terminated string that lives inside the mapped image. */
static int img_str(const uint8_t *img, uint32_t isz, uint32_t rva, char *out, size_t cap) {
    if (rva >= isz) return 0;
    for (size_t i = 0; rva + i < isz && i + 1 < cap; i++) {
        out[i] = (char)img[rva + i];
        if (!out[i]) return 1;
    }
    return 0;
}

int pe_load(Machine *m, const uint8_t *f, size_t n, char *err, size_t err_len) {
    if (n < 0x40 || f[0] != 'M' || f[1] != 'Z') FAIL("Not a PE file (missing MZ header)");
    uint32_t pe = u32(f + 0x3C);
    if ((uint64_t)pe + 24 > n || memcmp(f + pe, "PE\0\0", 4) != 0) FAIL("Not a PE file (missing PE signature)");

    uint16_t machine = u16(f + pe + 4);
    uint16_t nsec    = u16(f + pe + 6);
    uint16_t optsz   = u16(f + pe + 20);
    uint16_t chars   = u16(f + pe + 22);
    uint32_t opt     = pe + 24;
    if ((uint64_t)opt + optsz > n || optsz < 96) FAIL("Truncated optional header");

    uint16_t magic = u16(f + opt);
    if (magic == 0x20B) FAIL("64-bit (PE32+) executables are not supported yet - this build runs 32-bit x86 only");
    if (magic != 0x10B) FAIL("Unknown optional header magic 0x%X", magic);
    if (machine != 0x14C) FAIL("Unsupported machine type 0x%X (need x86)", machine);
    if (chars & 0x2000) FAIL("DLLs cannot be run directly");

    uint32_t entry_rva = u32(f + opt + 16);
    uint32_t pref_base = u32(f + opt + 28);
    uint32_t isize     = u32(f + opt + 56);
    uint32_t hsize     = u32(f + opt + 60);
    uint32_t ndd       = u32(f + opt + 92);

    if (isize == 0 || isize > 0x10000000u) FAIL("Unreasonable SizeOfImage 0x%X", isize);
    if (hsize > isize || hsize > n) FAIL("Bad SizeOfHeaders");
    if (entry_rva >= isize) FAIL("Entry point outside image");
    if (ndd > 16 || 96 + (uint64_t)ndd * 8 > optsz) FAIL("Bad data directory count");

    uint32_t imp_rva = 0, rel_rva = 0, rel_size = 0;
    if (ndd > 1) imp_rva = u32(f + opt + 96 + 8);
    if (ndd > 5) { rel_rva = u32(f + opt + 96 + 40); rel_size = u32(f + opt + 96 + 44); }

    uint32_t sec = opt + optsz;
    if ((uint64_t)sec + (uint64_t)nsec * 40 > n) FAIL("Truncated section table");

    /* pick a base: preferred if free, otherwise relocate */
    uint32_t span = (isize + 0xFFFu) & ~0xFFFu;
    static const uint32_t alt[] = { 0x00400000u, 0x01000000u, 0x02000000u, 0x04000000u, 0x08000000u };
    uint32_t base = 0;
    if (pref_base && !(pref_base & 0xFFFF) && range_free(m, pref_base, span)) base = pref_base;
    else if (rel_size) {
        for (size_t i = 0; i < sizeof alt / sizeof alt[0]; i++)
            if (range_free(m, alt[i], span)) { base = alt[i]; break; }
    }
    if (!base) FAIL("No free guest address range for the image (preferred base 0x%08X, no usable relocations)", pref_base);

    if (mem_commit(m, base, span) != 0) FAIL("Could not commit guest memory for the image");
    uint8_t *img = m->cpu.mem + base;

    memcpy(img, f, hsize);
    for (uint16_t i = 0; i < nsec; i++) {
        const uint8_t *s = f + sec + (size_t)i * 40;
        uint32_t vsize = u32(s + 8), va = u32(s + 12), rawsz = u32(s + 16), rawoff = u32(s + 20);
        uint32_t copy = rawsz;
        if (vsize && vsize < copy) copy = vsize;
        if (copy == 0) continue;
        if ((uint64_t)rawoff + copy > n) FAIL("Section %u raw data outside file", i);
        if ((uint64_t)va + copy > isize) FAIL("Section %u outside image", i);
        memcpy(img + va, f + rawoff, copy);
    }

    /* base relocations */
    uint32_t delta = base - pref_base;
    if (delta && rel_size) {
        if ((uint64_t)rel_rva + rel_size > isize) FAIL("Bad relocation directory");
        uint32_t off = 0;
        while (off + 8 <= rel_size) {
            uint32_t page = u32(img + rel_rva + off), bsz = u32(img + rel_rva + off + 4);
            if (bsz < 8 || off + bsz > rel_size) break;
            for (uint32_t e = 8; e + 2 <= bsz; e += 2) {
                uint16_t ent = u16(img + rel_rva + off + e);
                unsigned type = ent >> 12, o = ent & 0xFFF;
                if (type == 0) continue;
                if (type != 3) FAIL("Unsupported relocation type %u", type);
                if ((uint64_t)page + o + 4 > isize) FAIL("Relocation outside image");
                uint32_t v = u32(img + page + o) + delta;
                memcpy(img + page + o, &v, 4);
            }
            off += bsz;
        }
    }

    /* imports -> thunk addresses */
    if (imp_rva) {
        for (uint32_t d = imp_rva;; d += 20) {
            if ((uint64_t)d + 20 > isize) FAIL("Import directory runs off the image");
            uint32_t oft = u32(img + d), name_rva = u32(img + d + 12), ft = u32(img + d + 16);
            if (!name_rva && !ft) break;
            char dll[128];
            if (!img_str(img, isize, name_rva, dll, sizeof dll)) FAIL("Bad import DLL name");
            uint32_t lookup = oft ? oft : ft;
            for (uint32_t i = 0; i < 4096; i++) {
                uint32_t eo = lookup + i * 4, eo2 = ft + i * 4;
                if ((uint64_t)eo + 4 > isize || (uint64_t)eo2 + 4 > isize) FAIL("Import table runs off the image");
                uint32_t ent = u32(img + eo);
                if (!ent) break;
                char name[160];
                if (ent & 0x80000000u) snprintf(name, sizeof name, "#%u", ent & 0xFFFF);
                else if (!img_str(img, isize, (ent & 0x7FFFFFFFu) + 2, name, sizeof name)) FAIL("Bad import name");
                uint32_t addr = shim_resolve(m, dll, name);
                memcpy(img + eo2, &addr, 4);
            }
        }
    }

    m->image_base = base;
    m->image_size = isize;
    m->entry = base + entry_rva;

    /* returning from the entry point lands on ExitProcess */
    uint32_t esp = STACK_TOP - 16;
    esp -= 4;
    mem_w32(m, esp, shim_resolve(m, "kernel32.dll", "ExitProcess"));
    m->cpu.r[R_ESP] = esp;
    m->cpu.eip = m->entry;
    m->loaded = 1;
    return 0;
}
