#pragma once
#include "image.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <memory>
#include <string>
#include <vector>

struct LoadResult {
    std::unique_ptr<Image> img;
    ImageInfo info;
    std::string error;
    GdkPixbufAnimation* anim = nullptr;   // non-null for animated GIF/WebP (owned, unref when done)
    ~LoadResult() { if (anim) g_object_unref(anim); }
};

// Decode 'path'. Images with more than budget_px pixels are decoded at 1/N size.
std::unique_ptr<LoadResult> load_image(const std::string& path, uint64_t budget_px, LoadCtl& ctl);

// PSD / PSB (Photoshop) merged-composite decoder. Returns false + r.error on failure.
bool load_psd(const uint8_t* data, size_t len, uint64_t budget_px, LoadCtl& ctl, LoadResult& r);

// Copy a GdkPixbuf into a new Image (used for loaders and animation frames).
std::unique_ptr<Image> image_from_pixbuf(GdkPixbuf* pb);

std::string lower_ext(const std::string& path);
bool is_image_ext(const std::string& ext);
bool is_3d_ext(const std::string& ext);
