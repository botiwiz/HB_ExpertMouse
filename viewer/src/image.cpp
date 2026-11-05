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
    for (auto* s : l.tiles) if (s) cairo_surface_destroy(s);
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
            auto* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
            if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return false; }
            l.tiles[r * l.cols + c] = s;     // cairo zero-fills new image surfaces
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
        cairo_surface_t* s = l.tile(c, r);
        uint8_t* row = cairo_image_surface_get_data(s) + (size_t)ty * cairo_image_surface_get_stride(s);
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
        cairo_surface_t* s = l.tile(c, r);
        uint8_t* d = cairo_image_surface_get_data(s) + (size_t)ty * cairo_image_surface_get_stride(s) + tx * 4;
        int cnt = std::min(T - tx, end - x);
        const uint8_t* sp = src + (size_t)x * ch;
        if (ch == 4) memcpy(d, sp, (size_t)cnt * 4);
        else for (int i = 0; i < cnt; i++) { d[i*4] = sp[i*3]; d[i*4+1] = sp[i*3+1]; d[i*4+2] = sp[i*3+2]; d[i*4+3] = 255; }
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

void Image::finalize(RawLayout layout, bool alpha, bool unmatte, const uint8_t* pal) {
    has_alpha = alpha;
    Level& l = lv[0];
    parallel_for((int)l.tiles.size(), [&](int i) {
        cairo_surface_t* s = l.tiles[i];
        cairo_surface_flush(s);
        int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
        int stride = cairo_image_surface_get_stride(s);
        uint8_t* base = cairo_image_surface_get_data(s);
        for (int y = 0; y < h; y++) {
            uint8_t* p = base + (size_t)y * stride;
            for (int x = 0; x < w; x++, p += 4) {
                uint8_t r, g, b, a = 255;
                switch (layout) {
                case RawLayout::RGBA:  r = p[0]; g = p[1]; b = p[2]; if (alpha) a = p[3]; break;
                case RawLayout::Gray:  r = g = b = p[0]; break;
                case RawLayout::GrayA: r = g = b = p[0]; if (alpha) a = p[1]; break;
                case RawLayout::Indexed: r = pal[p[0]]; g = pal[256 + p[0]]; b = pal[512 + p[0]]; break;
                case RawLayout::CMYK:  // Photoshop stores CMYK inverted (255 = no ink)
                    r = (uint8_t)(p[0] * p[3] / 255); g = (uint8_t)(p[1] * p[3] / 255); b = (uint8_t)(p[2] * p[3] / 255); break;
                case RawLayout::Lab:
                case RawLayout::LabA: { uint8_t c[3]; lab_to_rgb(p[0], p[1], p[2], c); r = c[0]; g = c[1]; b = c[2];
                    if (alpha && layout == RawLayout::LabA) a = p[3]; break; }
                default: r = g = b = 0;
                }
                if (alpha && unmatte && a > 0 && a < 255) {
                    auto um = [a](uint8_t c) { int v = ((int)c - 255 + a) * 255 / a; return (uint8_t)std::clamp(v, 0, 255); };
                    r = um(r); g = um(g); b = um(b);
                }
                if (a == 255) { p[0] = b; p[1] = g; p[2] = r; p[3] = 255; }
                else { p[0] = (uint8_t)((b * a + 127) / 255); p[1] = (uint8_t)((g * a + 127) / 255);
                       p[2] = (uint8_t)((r * a + 127) / 255); p[3] = a; }
            }
        }
        cairo_surface_mark_dirty(s);
    });
}

void Image::mark_dirty() { for (auto* s : lv[0].tiles) cairo_surface_mark_dirty(s); }

void Image::build_mips() {
    while (lv.size() > 1) { free_level(lv.back()); lv.pop_back(); }
    while (std::max(lv.back().w, lv.back().h) > 256) {
        const Level& src = lv.back();
        Level dst;
        if (!make_level(dst, std::max(1, src.w / 2), std::max(1, src.h / 2))) { free_level(dst); break; }
        // dst tile (c,r) is fed by src tiles (2c..2c+1, 2r..2r+1): each one fills a quadrant
        parallel_for((int)dst.tiles.size(), [&](int i) {
            int c = i % dst.cols, r = i / dst.cols;
            cairo_surface_t* d = dst.tiles[i];
            uint8_t* db = cairo_image_surface_get_data(d);
            int ds = cairo_image_surface_get_stride(d);
            int dw = cairo_image_surface_get_width(d), dh = cairo_image_surface_get_height(d);
            for (int q = 0; q < 4; q++) {
                int sc = 2 * c + (q & 1), sr = 2 * r + (q >> 1);
                if (sc >= src.cols || sr >= src.rows) continue;
                cairo_surface_t* s = src.tile(sc, sr);
                const uint8_t* sb = cairo_image_surface_get_data(s);
                int ss = cairo_image_surface_get_stride(s);
                int sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
                int ox = (q & 1) * (T / 2), oy = (q >> 1) * (T / 2);
                for (int y = 0; y < sh / 2 && oy + y < dh; y++) {
                    const uint8_t* s0 = sb + (size_t)(2 * y) * ss;
                    const uint8_t* s1 = s0 + ss;
                    uint8_t* dp = db + (size_t)(oy + y) * ds + ox * 4;
                    for (int x = 0; x < sw / 2 && ox + x < dw; x++, dp += 4, s0 += 8, s1 += 8)
                        for (int k = 0; k < 4; k++) dp[k] = (uint8_t)((s0[k] + s0[k + 4] + s1[k] + s1[k + 4] + 2) >> 2);
                }
            }
            cairo_surface_mark_dirty(d);
        });
        lv.push_back(std::move(dst));
    }
}

bool Image::pixel(int x, int y, uint8_t o[4]) const {
    if (lv.empty() || x < 0 || y < 0 || x >= lv[0].w || y >= lv[0].h) return false;
    cairo_surface_t* s = lv[0].tile(x / T, y / T);
    const uint8_t* p = cairo_image_surface_get_data(s) + (size_t)(y % T) * cairo_image_surface_get_stride(s) + (x % T) * 4;
    uint8_t a = p[3];
    auto un = [a](uint8_t c) { return a ? (uint8_t)std::min(255, (c * 255 + a / 2) / a) : (uint8_t)0; };
    o[0] = un(p[2]); o[1] = un(p[1]); o[2] = un(p[0]); o[3] = a;
    return true;
}
