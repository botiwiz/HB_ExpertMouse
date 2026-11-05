// Photoshop PSD/PSB reader. Reads only the merged composite (the "Maximize compatibility"
// image), so layer data - usually most of a huge file - is skipped entirely. The file is
// memory-mapped, rows are decoded in parallel bands, and images over the memory budget are
// box-filtered down while decoding, so even multi-GB PSBs open with modest RAM.
#include "decode.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>
#include <cstdio>

namespace {

struct Reader {
    const uint8_t* d; size_t n, p = 0; bool bad = false;
    bool need(size_t k) { if (p + k > n) { bad = true; return false; } return true; }
    uint64_t be(int k) { if (!need(k)) return 0; uint64_t v = 0; for (int i = 0; i < k; i++) v = v << 8 | d[p++]; return v; }
    void skip(uint64_t k) { if (k > n - p) { bad = true; p = n; } else p += k; }
};

// PackBits; returns false on malformed input.
bool unpackbits(const uint8_t* s, size_t slen, uint8_t* dst, size_t dlen) {
    size_t i = 0, o = 0;
    while (o < dlen && i < slen) {
        int8_t c = (int8_t)s[i++];
        if (c >= 0) {
            size_t k = (size_t)c + 1;
            if (i + k > slen) k = slen - i;
            if (o + k > dlen) k = dlen - o;
            memcpy(dst + o, s + i, k); i += k; o += k;
        } else if (c != -128) {
            size_t k = (size_t)(1 - c);
            if (i >= slen) break;
            if (o + k > dlen) k = dlen - o;
            memset(dst + o, s[i++], k); o += k;
        }
    }
    if (o < dlen) { memset(dst + o, 0, dlen - o); return false; }
    return true;
}

const char* mode_name(int m) {
    static const char* names[] = {"Bitmap", "Grayscale", "Indexed", "RGB", "CMYK", "?", "?", "Multichannel", "Duotone", "Lab"};
    return m >= 0 && m <= 9 ? names[m] : "?";
}

} // namespace

bool load_psd(const uint8_t* data, size_t len, uint64_t budget, LoadCtl& ctl, LoadResult& r, bool fast) {
    Reader rd{data, len};
    if (rd.be(4) != 0x38425053) { r.error = "not a PSD"; return false; }   // "8BPS"
    int ver = (int)rd.be(2);
    if (ver != 1 && ver != 2) { r.error = "unknown PSD version"; return false; }
    bool psb = ver == 2;
    rd.skip(6);
    int nch = (int)rd.be(2);
    uint32_t h = (uint32_t)rd.be(4), w = (uint32_t)rd.be(4);
    int depth = (int)rd.be(2), mode = (int)rd.be(2);
    if (rd.bad || !w || !h || nch < 1 || nch > 56 || (depth != 1 && depth != 8 && depth != 16 && depth != 32)) {
        r.error = "unsupported PSD header"; return false;
    }

    // colour mode data (palette for indexed images)
    uint32_t cmlen = (uint32_t)rd.be(4);
    uint8_t pal[768] = {};
    if (mode == 2 && cmlen >= 768 && rd.need(768)) memcpy(pal, data + rd.p, 768);
    rd.skip(cmlen);
    rd.skip(rd.be(4));                                       // image resources

    // layer & mask info: we only need the layer count's sign (negative = first extra
    // channel of the merged image is its transparency)
    uint64_t lmlen = rd.be(psb ? 8 : 4);
    size_t lm_start = rd.p;
    int layer_count = 0;
    if (lmlen >= (psb ? 10u : 6u)) {
        uint64_t lilen = rd.be(psb ? 8 : 4);
        if (lilen >= 2) layer_count = (int16_t)rd.be(2);
    }
    rd.p = lm_start; rd.skip(lmlen);
    if (rd.bad) { r.error = "truncated PSD"; return false; }

    int comp = (int)rd.be(2);
    if (comp != 0 && comp != 1) { r.error = "unsupported PSD composite compression " + std::to_string(comp); return false; }

    int nbase; RawLayout layout;
    switch (mode) {
    case 0: nbase = 1; layout = RawLayout::Gray; break;
    case 1: case 7: case 8: nbase = 1; layout = RawLayout::GrayA; break;
    case 2: nbase = 1; layout = RawLayout::Indexed; break;
    case 3: nbase = 3; layout = RawLayout::RGBA; break;
    case 4: nbase = 4; layout = RawLayout::CMYK; break;
    case 9: nbase = 3; layout = RawLayout::LabA; break;
    default: r.error = "unsupported PSD colour mode"; return false;
    }
    if (mode == 7) nbase = 1;                                 // multichannel: show first channel
    if (nch < nbase) { if (mode == 3 && nch >= 1) { nbase = 1; layout = RawLayout::GrayA; } else { r.error = "too few channels"; return false; } }
    bool alpha = nch > nbase && layer_count < 0 && (mode == 1 || mode == 3 || mode == 9 || mode == 8);
    int ndec = std::min(nbase + (alpha ? 1 : 0), 4);

    size_t rowbytes = ((size_t)w * depth + 7) / 8;
    std::vector<uint64_t> off((size_t)nch * h), sz((size_t)nch * h);
    size_t data0 = rd.p;
    if (comp == 1) {
        int cb = psb ? 4 : 2;
        if (!rd.need((size_t)nch * h * cb)) { r.error = "truncated PSD RLE table"; return false; }
        uint64_t pos = data0 + (uint64_t)nch * h * cb;
        for (size_t i = 0; i < (size_t)nch * h; i++) {
            sz[i] = rd.be(cb); off[i] = pos; pos += sz[i];
        }
        // the table is only needed for the channels we decode
        if (off[(size_t)ndec * h - 1] + sz[(size_t)ndec * h - 1] > len) { r.error = "truncated PSD image data"; return false; }
    } else {
        for (size_t i = 0; i < (size_t)nch * h; i++) { off[i] = data0 + i * rowbytes; sz[i] = rowbytes; }
        if (data0 + (uint64_t)ndec * h * rowbytes > len) { r.error = "truncated PSD image data"; return false; }
    }

    // memory budget -> integer downscale factor
    int s = 1;
    while ((uint64_t)((w + s - 1) / s) * ((h + s - 1) / s) > budget) s++;
    int ow = (int)((w + s - 1) / s), oh = (int)((h + s - 1) / s);

    auto T0 = std::chrono::steady_clock::now();
    auto TM = [&](const char* w) { if (getenv("HBV_TIMING")) fprintf(stderr, "  %s %.3f\n", w, std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count()); };
    r.img = std::make_unique<Image>();
    if (!r.img->alloc(ow, oh)) { r.error = "out of memory"; r.img.reset(); return false; }

    // 32-bit PSDs are linear light: encode to sRGB for display
    std::vector<uint8_t> lin2srgb;
    if (depth == 32) {
        lin2srgb.resize(4097);
        for (int i = 0; i <= 4096; i++) { float c = i / 4096.f;
            lin2srgb[i] = (uint8_t)std::lround(255 * (c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1 / 2.4f) - 0.055f)); }
    }

    if (mode == 0 || mode == 2) alpha = false;
    std::atomic<uint64_t> done_rows{0};
    uint64_t total_rows = (uint64_t)oh;
    unsigned nthreads = hw_threads();
    int nbands = std::max(1, std::min(oh, (int)nthreads * 8));
    std::atomic<int> next_band{0};

    // Each output row: decode (s input rows of) every channel, box-filter, convert to
    // premultiplied BGRA and write it once. 'fast' (preview) samples one input row per s.
    auto worker = [&] {
        std::vector<uint8_t> packed(rowbytes), line(w), bgra((size_t)ow * 4);
        std::vector<uint8_t> chl[4];
        for (auto& c : chl) c.assign(ow, 0);
        std::vector<uint32_t> acc(ow);
        const uint8_t* chp[4] = {chl[0].data(), chl[1].data(), chl[2].data(), chl[3].data()};
        for (int b; (b = next_band++) < nbands && !ctl.cancel;) {
            int oy0 = (int)((int64_t)oh * b / nbands), oy1 = (int)((int64_t)oh * (b + 1) / nbands);
            for (int oy = oy0; oy < oy1 && !ctl.cancel; oy++) {
                int y0 = oy * s, y1 = std::min<int>(y0 + s, (int)h);
                if (fast) y1 = y0 + 1;
                for (int ch = 0; ch < ndec; ch++) {
                    bool is_alpha = ch >= nbase;
                    uint8_t* dst = s == 1 ? chl[ch].data() : line.data();
                    if (s > 1) std::fill(acc.begin(), acc.end(), 0);
                    for (int y = y0; y < y1; y++) {
                        size_t i = (size_t)ch * h + y;
                        const uint8_t* src = data + off[i];
                        const uint8_t* row;
                        if (comp == 1) {
                            if (depth == 8) { unpackbits(src, sz[i], dst, w); row = dst; }
                            else { unpackbits(src, sz[i], packed.data(), rowbytes); row = packed.data(); }
                        } else row = src;
                        switch (depth) {
                        case 1: for (uint32_t x = 0; x < w; x++) dst[x] = (row[x >> 3] >> (7 - (x & 7)) & 1) ? 0 : 255; break;
                        case 8: if (row != dst) memcpy(dst, row, w); break;
                        case 16: for (uint32_t x = 0; x < w; x++) dst[x] = row[2 * x]; break;
                        case 32: for (uint32_t x = 0; x < w; x++) {
                            uint32_t u = (uint32_t)row[4*x] << 24 | row[4*x+1] << 16 | row[4*x+2] << 8 | row[4*x+3];
                            float f; memcpy(&f, &u, 4);
                            f = std::isfinite(f) ? std::clamp(f, 0.f, 1.f) : 0.f;
                            dst[x] = is_alpha ? (uint8_t)std::lround(f * 255) : lin2srgb[(int)(f * 4096)];
                        } break;
                        }
                        if (s > 1)
                            for (int ox = 0, x0 = 0; ox < ow; ox++, x0 += s) {
                                uint32_t sum = 0; int x1 = std::min<int>(x0 + s, (int)w);
                                for (int x = x0; x < x1; x++) sum += dst[x];
                                acc[ox] += sum;
                            }
                    }
                    if (s > 1) {
                        int ny = y1 - y0;
                        for (int ox = 0; ox < ow; ox++) {
                            int n = (std::min<int>(ox * s + s, (int)w) - ox * s) * ny;
                            chl[ch][ox] = (uint8_t)((acc[ox] + n / 2) / n);
                        }
                    }
                }
                convert_pixels(layout, alpha, alpha, mode == 2 ? pal : nullptr, chp, 1, bgra.data(), ow);
                r.img->put_bgra(oy, bgra.data(), ow);
                ctl.progress = (int)(++done_rows * 950 / total_rows);
            }
        }
    };
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nthreads; t++) th.emplace_back(worker);
    TM("alloc");
    for (auto& t : th) t.join();
    TM("decode");
    if (ctl.cancel) { r.img.reset(); r.error = "cancelled"; return false; }
    r.img->has_alpha = alpha;
    for (auto* t : r.img->lv[0].tiles) cairo_surface_mark_dirty(t);
    r.img->build_mips();
    TM("mips");
    ctl.progress = 1000;

    r.info.format = psb ? "PSB" : "PSD";
    r.info.orig_w = (int)w; r.info.orig_h = (int)h; r.info.scale_div = s; r.info.preview = fast;
    r.info.details = std::string(mode_name(mode)) + ", " + std::to_string(depth) + "-bit, " +
                     std::to_string(nch) + " ch" + (alpha ? " (alpha)" : "") +
                     ", " + std::to_string(std::abs(layer_count)) + " layers" + (comp ? ", RLE" : ", raw");
    return true;
}
