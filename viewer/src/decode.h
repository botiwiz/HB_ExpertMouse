#pragma once
#include "image.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QImage;

struct LoadResult {
    std::unique_ptr<Image> img;
    ImageInfo info;
    std::string error;
    bool animated = false;          // multi-frame GIF/WebP/APNG...: the viewer plays it with QMovie
};

// Decode 'path' (thread-safe). Images with more than budget_px pixels are decoded at 1/N size.
// on_preview (optional) receives a quick low-res result first for very large PSD/PSB files.
std::unique_ptr<LoadResult> load_image(const std::string& path, uint64_t budget_px, LoadCtl& ctl,
                                       const std::function<void(std::unique_ptr<LoadResult>)>& on_preview = nullptr);

// PSD / PSB (Photoshop) merged-composite decoder. Returns false + r.error on failure.
bool load_psd(const uint8_t* data, size_t len, uint64_t budget_px, LoadCtl& ctl, LoadResult& r, bool fast = false);

// QImage <-> Image (straight / premultiplied handled internally)
std::unique_ptr<Image> image_from_qimage(const QImage& q);
QImage qimage_from_image(Image* im);     // full level-0 copy; null QImage if too large

std::string lower_ext(const std::string& path);
bool is_image_ext(const std::string& ext);
bool is_3d_ext(const std::string& ext);
bool is_raw_ext(const std::string& ext);
