// Tiled, mip-mapped RGBA image. Tiles avoid 32k-pixel image limits and let us
// draw only what is visible; mip levels make zoomed-out views of huge images fast.
#pragma once
#include <cstdlib>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// How bytes 0..3 of each freshly decoded pixel should be interpreted by finalize().
enum class RawLayout { RGBA, Gray, GrayA, Indexed, CMYK, Lab, LabA };

// One tile: premultiplied 32-bit pixels, bytes B,G,R,A (= QImage::Format_ARGB32_Premultiplied
// on little-endian), zero-initialised.
struct Tile {
    int w = 0, h = 0, stride = 0;
    uint8_t* data = nullptr;
    Tile(int w_, int h_) : w(w_), h(h_), stride(w_ * 4), data((uint8_t*)calloc((size_t)w_ * h_, 4)) {}
    ~Tile() { free(data); }
    Tile(const Tile&) = delete;
    Tile& operator=(const Tile&) = delete;
};

struct Level {
    int w = 0, h = 0, cols = 0, rows = 0;
    std::vector<Tile*> tiles;   // row-major, cols*rows
    Tile* tile(int c, int r) const { return tiles[r * cols + c]; }
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
    // Write n already-converted (premultiplied BGRA) pixels into row y of level 0.
    void put_bgra(int y, const uint8_t* src, int n);
    // Write n RGBA (or RGB when ch==3) pixels into row y of level 0.
    void put_rgba(int y, const uint8_t* src, int n, int ch);

    // Convert raw bytes to premultiplied ARGB32, optionally un-matting
    // (Photoshop stores merged transparency composited over white).
    void finalize(RawLayout layout, bool has_alpha, bool unmatte_white, const uint8_t* palette768 = nullptr);
    void build_mips();                      // call after finalize()

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
    bool preview = false;      // quick low-res pass; full decode still running
};

struct LoadCtl {
    std::atomic<bool> cancel{false};
    std::atomic<int> progress{0};    // 0..1000
};

unsigned hw_threads();

// Convert one row of raw channel bytes (ch[k][x*stride]) to premultiplied BGRA.
void convert_pixels(RawLayout layout, bool alpha, bool unmatte, const uint8_t* pal,
                    const uint8_t* const ch[4], int stride, uint8_t* out, int n);
