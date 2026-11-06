#include "image.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <thread>

unsigned hw_threads() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? std::min(n, 32u) : 4u;
}

// Run fn(i) for i in [0,n) on all cores.
static void parallel_for(int n, const std::function<void(int)>& fn) {
    unsigned nt = std::min<unsigned>(hw_threads(), (unsigned)std::max(n, 1));
    if (nt <= 1) { for (int i = 0; i < n; i++) fn(i); return; }
    std::atomic<int> next{0};
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; t++)
        th.emplace_back([&] { for (int i; (i = next++) < n;) fn(i); });
    for (auto& t : th) t.join();
}

static void free_level(Level& l) {
    for (auto* s : l.tiles) delete s;
    l.tiles.clear();
}

Image::~Image() { for (auto& l : lv) free_level(l); }

static bool make_level(Level& l, int w, int h) {
    l.w = w; l.h = h;
    l.cols = (w + Image::T - 1) / Image::T;
    l.rows = (h + Image::T - 1) / Image::T;
    l.tiles.assign((size_t)l.cols * l.rows, nullptr);
    for (int r = 0; r < l.rows; r++)
        for (int c = 0; c < l.cols; c++) {
            int tw = std::min(Image::T, w - c * Image::T), th = std::min(Image::T, h - r * Image::T);
            auto* s = new Tile(tw, th);
            if (!s->data) { delete s; return false; }
            l.tiles[r * l.cols + c] = s;
        }
    return true;
}

bool Image::alloc(int w, int h) {
    for (auto& l : lv) free_level(l);
    lv.clear();
    if (w <= 0 || h <= 0) return false;
    lv.emplace_back();
    if (!make_level(lv[0], w, h)) { free_level(lv[0]); lv.clear(); return false; }
    return true;
}

size_t Image::bytes() const {
    size_t b = 0;
    for (auto& l : lv) b += (size_t)l.w * l.h * 4;
    return b;
}

void Image::put_channel(int y, int x0, int slot, const uint8_t* src, int n) {
    const Level& l = lv[0];
    int r = y / T, ty = y % T;
    int x = x0, end = std::min(x0 + n, l.w);
    while (x < end) {
        int c = x / T, tx = x % T;
        Tile* s = l.tile(c, r);
        uint8_t* row = s->data + (size_t)ty * s->stride;
        int cnt = std::min(T - tx, end - x);
        uint8_t* d = row + tx * 4 + slot;
        const uint8_t* sp = src + (x - x0);
        for (int i = 0; i < cnt; i++) d[i * 4] = sp[i];
        x += cnt;
    }
}

void Image::put_rgba(int y, const uint8_t* src, int n, int ch) {
    const Level& l = lv[0];
    int r = y / T, ty = y % T;
    int x = 0, end = std::min(n, l.w);
    while (x < end) {
        int c = x / T, tx = x % T;
        Tile* s = l.tile(c, r);
        uint8_t* d = s->data + (size_t)ty * s->stride + tx * 4;
        int cnt = std::min(T - tx, end - x);
        const uint8_t* sp = src + (size_t)x * ch;
        if (ch == 4) memcpy(d, sp, (size_t)cnt * 4);
        else
            for (int i = 0; i < cnt; i++) { d[i*4] = sp[i*3]; d[i*4+1] = sp[i*3+1]; d[i*4+2] = sp[i*3+2]; d[i*4+3] = 255; }
        x += cnt;
    }
}

static inline uint8_t clamp8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

static void lab_to_rgb(uint8_t L8, uint8_t A8, uint8_t B8, uint8_t* rgb) {
    float L = L8 * (100.f / 255.f), a = A8 - 128.f, b = B8 - 128.f;
    float fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    auto finv = [](float t) { return t > 6.f / 29 ? t * t * t : 3 * (6.f / 29) * (6.f / 29) * (t - 4.f / 29); };
    float X = 0.9642f * finv(fx), Y = finv(fy), Z = 0.8249f * finv(fz);     // D50
    // Bradford-adapted XYZ(D50) -> linear sRGB
    float r =  3.1339f * X - 1.6169f * Y - 0.4906f * Z;
    float g = -0.9788f * X + 1.9161f * Y + 0.0335f * Z;
    float bl = 0.0719f * X - 0.2290f * Y + 1.4052f * Z;
    auto enc = [](float c) { c = c < 0 ? 0 : c > 1 ? 1 : c; return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1 / 2.4f) - 0.055f; };
    rgb[0] = clamp8(enc(r) * 255); rgb[1] = clamp8(enc(g) * 255); rgb[2] = clamp8(enc(bl) * 255);
}

void convert_pixels(RawLayout layout, bool alpha, bool unmatte, const uint8_t* pal,
                    const uint8_t* const ch[4], int stride, uint8_t* out, int n) {
    // fast paths for the common cases
    if (layout == RawLayout::RGBA && !alpha) {
        const uint8_t *r = ch[0], *g = ch[1], *b = ch[2];
        uint32_t* o = (uint32_t*)out;
        if (stride == 1) for (int x = 0; x < n; x++) o[x] = 0xFF000000u | (uint32_t)r[x] << 16 | (uint32_t)g[x] << 8 | b[x];
        else for (int x = 0; x < n; x++) { size_t i = (size_t)x * stride; o[x] = 0xFF000000u | (uint32_t)r[i] << 16 | (uint32_t)g[i] << 8 | b[i]; }
        return;
    }
    if ((layout == RawLayout::Gray || layout == RawLayout::GrayA) && !alpha && stride == 1) {
        uint32_t* o = (uint32_t*)out;
        for (int x = 0; x < n; x++) o[x] = 0xFF000000u | ch[0][x] * 0x010101u;
        return;
    }
    for (int x = 0; x < n; x++) {
        size_t o = (size_t)x * stride;
        uint8_t r, g, b, a = 255;
        switch (layout) {
        case RawLayout::RGBA:  r = ch[0][o]; g = ch[1][o]; b = ch[2][o]; if (alpha) a = ch[3][o]; break;
        case RawLayout::Gray:  r = g = b = ch[0][o]; break;
        case RawLayout::GrayA: r = g = b = ch[0][o]; if (alpha) a = ch[1][o]; break;
        case RawLayout::Indexed: { uint8_t i = ch[0][o]; r = pal[i]; g = pal[256 + i]; b = pal[512 + i]; break; }
        case RawLayout::CMYK: {  // Photoshop stores CMYK inverted (255 = no ink)
            unsigned k = ch[3][o];
            r = (uint8_t)(ch[0][o] * k / 255); g = (uint8_t)(ch[1][o] * k / 255); b = (uint8_t)(ch[2][o] * k / 255); break; }
        case RawLayout::Lab:
        case RawLayout::LabA: { uint8_t c[3]; lab_to_rgb(ch[0][o], ch[1][o], ch[2][o], c); r = c[0]; g = c[1]; b = c[2];
            if (alpha && layout == RawLayout::LabA) a = ch[3][o];
            break; }
        default: r = g = b = 0;
        }
        if (alpha && unmatte && a > 0 && a < 255) {
            auto um = [a](uint8_t c) { int v = ((int)c - 255 + a) * 255 / a; return (uint8_t)std::clamp(v, 0, 255); };
            r = um(r); g = um(g); b = um(b);
        }
        uint8_t* p = out + (size_t)x * 4;
        if (a == 255) { p[0] = b; p[1] = g; p[2] = r; p[3] = 255; }
        else { p[0] = (uint8_t)((b * a + 127) / 255); p[1] = (uint8_t)((g * a + 127) / 255);
               p[2] = (uint8_t)((r * a + 127) / 255); p[3] = a; }
    }
}

void Image::put_bgra(int y, const uint8_t* src, int n) {
    const Level& l = lv[0];
    int r = y / T, ty = y % T;
    for (int x = 0, end = std::min(n, l.w); x < end;) {
        int c = x / T, tx = x % T, cnt = std::min(T - tx, end - x);
        Tile* s = l.tile(c, r);
        memcpy(s->data + (size_t)ty * s->stride + tx * 4, src + (size_t)x * 4, (size_t)cnt * 4);
        x += cnt;
    }
}

void Image::finalize(RawLayout layout, bool alpha, bool unmatte, const uint8_t* pal) {
    has_alpha = alpha;
    Level& l = lv[0];
    parallel_for((int)l.tiles.size(), [&](int i) {
        Tile* s = l.tiles[i];
        int w = s->w, h = s->h;
        int stride = s->stride;
        uint8_t* base = s->data;
        for (int y = 0; y < h; y++) {
            uint8_t* p = base + (size_t)y * stride;
            const uint8_t* ch[4] = {p, p + 1, p + 2, p + 3};
            convert_pixels(layout, alpha, unmatte, pal, ch, 4, p, w);   // in place: reads a pixel before writing it
        }
    });
}


void Image::build_mips() {
    while (lv.size() > 1) { free_level(lv.back()); lv.pop_back(); }
    while (std::max(lv.back().w, lv.back().h) > 256) {
        const Level& src = lv.back();
        Level dst;
        if (!make_level(dst, std::max(1, src.w / 2), std::max(1, src.h / 2))) { free_level(dst); break; }
        // dst tile (c,r) is fed by src tiles (2c..2c+1, 2r..2r+1): each one fills a quadrant
        parallel_for((int)dst.tiles.size(), [&](int i) {
            int c = i % dst.cols, r = i / dst.cols;
            Tile* d = dst.tiles[i];
            uint8_t* db = d->data;
            int ds = d->stride;
            int dw = d->w, dh = d->h;
            for (int q = 0; q < 4; q++) {
                int sc = 2 * c + (q & 1), sr = 2 * r + (q >> 1);
                if (sc >= src.cols || sr >= src.rows) continue;
                Tile* s = src.tile(sc, sr);
                const uint8_t* sb = s->data;
                int ss = s->stride;
                int sw = s->w, sh = s->h;
                int ox = (q & 1) * (T / 2), oy = (q >> 1) * (T / 2);
                for (int y = 0; y < sh / 2 && oy + y < dh; y++) {
                    const uint8_t* s0 = sb + (size_t)(2 * y) * ss;
                    const uint8_t* s1 = s0 + ss;
                    uint8_t* dp = db + (size_t)(oy + y) * ds + ox * 4;
                    const uint32_t* a0 = (const uint32_t*)s0; const uint32_t* a1 = (const uint32_t*)s1;
                    uint32_t* o = (uint32_t*)dp;
                    int nx = std::min(sw / 2, dw - ox);
                    for (int x = 0; x < nx; x++) {      // average 2x2 pixels, two 8-bit lanes per 32-bit op
                        uint32_t p0 = a0[2 * x], p1 = a0[2 * x + 1], p2 = a1[2 * x], p3 = a1[2 * x + 1];
                        uint32_t lo = (p0 & 0x00FF00FF) + (p1 & 0x00FF00FF) + (p2 & 0x00FF00FF) + (p3 & 0x00FF00FF) + 0x00020002;
                        uint32_t hi = (p0 >> 8 & 0x00FF00FF) + (p1 >> 8 & 0x00FF00FF) + (p2 >> 8 & 0x00FF00FF) + (p3 >> 8 & 0x00FF00FF) + 0x00020002;
                        o[x] = (lo >> 2 & 0x00FF00FF) | (hi >> 2 & 0x00FF00FF) << 8;
                    }
                }
            }
        });
        lv.push_back(std::move(dst));
    }
}

bool Image::pixel(int x, int y, uint8_t o[4]) const {
    if (lv.empty() || x < 0 || y < 0 || x >= lv[0].w || y >= lv[0].h) return false;
    Tile* s = lv[0].tile(x / T, y / T);
    const uint8_t* p = s->data + (size_t)(y % T) * s->stride + (x % T) * 4;
    uint8_t a = p[3];
    auto un = [a](uint8_t c) { return a ? (uint8_t)std::min(255, (c * 255 + a / 2) / a) : (uint8_t)0; };
    o[0] = un(p[2]); o[1] = un(p[1]); o[2] = un(p[0]); o[3] = a;
    return true;
}
