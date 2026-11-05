// Tiled, mip-mapped RGBA image. Tiles avoid cairo's 32767px surface limit and let us
// draw only what is visible; mip levels make zoomed-out views of huge images fast.
#pragma once
#include <cairo.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// How bytes 0..3 of each freshly decoded pixel should be interpreted by finalize().
enum class RawLayout { RGBA, Gray, GrayA, Indexed, CMYK, Lab, LabA };

struct Level {
    int w = 0, h = 0, cols = 0, rows = 0;
    std::vector<cairo_surface_t*> tiles;   // row-major, cols*rows
    cairo_surface_t* tile(int c, int r) const { return tiles[r * cols + c]; }
};

class Image {
public:
    static constexpr int T = 2048;          // tile edge (power of two, mips rely on it)

    ~Image();
    bool alloc(int w, int h);               // level 0, zero-filled
    int width() const { return lv.empty() ? 0 : lv[0].w; }
    int height() const { return lv.empty() ? 0 : lv[0].h; }
    size_t bytes() const;

    // Write src[0..n) into byte 'slot' (0..3) of pixels (x0.., y) of level 0.
    void put_channel(int y, int x0, int slot, const uint8_t* src, int n);
    // Write n RGBA (or RGB when ch==3) pixels into row y of level 0.
    void put_rgba(int y, const uint8_t* src, int n, int ch);

    // Convert raw bytes to cairo's premultiplied ARGB32, optionally un-matting
    // (Photoshop stores merged transparency composited over white).
    void finalize(RawLayout layout, bool has_alpha, bool unmatte_white, const uint8_t* palette768 = nullptr);
    void build_mips();                      // call after finalize()
    void mark_dirty();                      // after editing level-0 tile data directly

    // Read one premultiplied pixel of level 0 as straight RGBA.
    bool pixel(int x, int y, uint8_t rgba[4]) const;

    std::vector<Level> lv;
    bool has_alpha = false;
};

// Information about a decoded file, shown in the status bar / info dialog.
struct ImageInfo {
    std::string format;        // "PSD", "PSB", "PNG", ...
    std::string details;       // colour mode, depth, layers ...
    int orig_w = 0, orig_h = 0;
    int scale_div = 1;         // decoded at 1/scale_div because of the memory budget
};

struct LoadCtl {
    std::atomic<bool> cancel{false};
    std::atomic<int> progress{0};    // 0..1000
};

unsigned hw_threads();
