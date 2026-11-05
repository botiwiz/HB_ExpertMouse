#include "decode.h"
#include <glib.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#define STBI_NO_PSD            // our own reader handles PSD/PSB fully
#define STBI_NO_STDIO
#include "stb_image.h"

std::string lower_ext(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    for (auto& c : e) c = (char)g_ascii_tolower(c);
    return e;
}

static const std::set<std::string> kRaw = {"3fr","arw","cr2","cr3","crw","dcr","dng","erf","kdc","mef","mos","mrw",
    "nef","nrw","orf","pef","raf","raw","rw2","rwl","sr2","srf","srw","x3f","iiq"};
static const std::set<std::string> kStb = {"hdr","pic","pnm","ppm","pgm","pbm","tga"};
static const std::set<std::string> k3d = {"obj","stl","ply","gltf","glb","fbx","3ds","dae","off","vtk","vtp","vtu","vtr",
    "vti","vts","vtm","step","stp","iges","igs","brep","usd","usda","usdc","usdz","abc","3mf","x3d","drc","splat","spz","pts","e57","las"};

bool is_3d_ext(const std::string& e) { return k3d.count(e) > 0; }

bool is_image_ext(const std::string& e) {
    static std::set<std::string> exts;
    if (exts.empty()) {
        exts = {"psd", "psb"};
        exts.insert(kRaw.begin(), kRaw.end());
        exts.insert(kStb.begin(), kStb.end());
        GSList* fl = gdk_pixbuf_get_formats();
        for (GSList* l = fl; l; l = l->next) {
            gchar** ex = gdk_pixbuf_format_get_extensions((GdkPixbufFormat*)l->data);
            for (gchar** p = ex; p && *p; p++) exts.insert(*p);
            g_strfreev(ex);
        }
        g_slist_free(fl);
        exts.erase("ani");   // cursors: rarely wanted while browsing
    }
    return exts.count(e) > 0;
}

std::unique_ptr<Image> image_from_pixbuf(GdkPixbuf* pb) {
    auto img = std::make_unique<Image>();
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    if (!img->alloc(w, h)) return nullptr;
    int ch = gdk_pixbuf_get_n_channels(pb), rs = gdk_pixbuf_get_rowstride(pb);
    const guint8* px = gdk_pixbuf_read_pixels(pb);
    for (int y = 0; y < h; y++) img->put_rgba(y, px + (size_t)y * rs, w, ch);
    img->finalize(RawLayout::RGBA, gdk_pixbuf_get_has_alpha(pb), false);
    return img;
}

GdkPixbuf* pixbuf_from_image(Image* im) {   // straight alpha
    if (!im || (uint64_t)im->width() * im->height() * 4 >= INT32_MAX) return nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, im->width(), im->height());
    if (!pb) return nullptr;
    guint8* px = gdk_pixbuf_get_pixels(pb); int rs = gdk_pixbuf_get_rowstride(pb);
    for (int y = 0; y < im->height(); y++)
        for (int x = 0; x < im->width(); x++) im->pixel(x, y, px + (size_t)y * rs + x * 4);
    return pb;
}

bool is_raw_ext(const std::string& e) { return kRaw.count(e) > 0; }

static std::string pixbuf_format_name(const char* path) {
    GdkPixbufFormat* f = gdk_pixbuf_get_file_info(path, nullptr, nullptr);
    if (!f) return "";
    gchar* n = gdk_pixbuf_format_get_name(f);
    std::string s = n ? n : ""; g_free(n);
    for (auto& c : s) c = (char)g_ascii_toupper(c);
    return s;
}

static bool load_pixbuf(const std::string& path, uint64_t budget, LoadResult& r) {
    std::string ext = lower_ext(path);
    if (ext == "gif" || ext == "webp" || ext == "png" || ext == "apng") {
        GdkPixbufAnimation* a = gdk_pixbuf_animation_new_from_file(path.c_str(), nullptr);
        if (a && !gdk_pixbuf_animation_is_static_image(a)) {
            r.anim = a;
            GdkPixbuf* f = gdk_pixbuf_animation_get_static_image(a);
            r.img = image_from_pixbuf(f);
            if (!r.img) { r.error = "out of memory"; return false; }
            r.img->build_mips();
            r.info.format = pixbuf_format_name(path.c_str()) + " (animated)";
            r.info.orig_w = r.img->width(); r.info.orig_h = r.img->height();
            return true;
        }
        if (a) g_object_unref(a);
    }
    int w = 0, h = 0;
    if (!gdk_pixbuf_get_file_info(path.c_str(), &w, &h)) { r.error = "unknown format"; return false; }
    int s = 1;
    if (w > 0 && h > 0) while ((uint64_t)(w / s) * (h / s) > budget) s++;
    GError* err = nullptr;
    GdkPixbuf* pb = s > 1 ? gdk_pixbuf_new_from_file_at_scale(path.c_str(), w / s, h / s, TRUE, &err)
                          : gdk_pixbuf_new_from_file(path.c_str(), &err);
    if (!pb) { r.error = err ? err->message : "decode failed"; if (err) g_error_free(err); return false; }
    GdkPixbuf* o = gdk_pixbuf_apply_embedded_orientation(pb);
    g_object_unref(pb);
    r.img = image_from_pixbuf(o);
    r.info.format = pixbuf_format_name(path.c_str());
    r.info.details = std::string(gdk_pixbuf_get_has_alpha(o) ? "RGBA" : "RGB") + ", " +
                     std::to_string(gdk_pixbuf_get_bits_per_sample(o)) + "-bit decoded";
    g_object_unref(o);
    if (!r.img) { r.error = "out of memory"; return false; }
    r.img->build_mips();
    r.info.orig_w = w; r.info.orig_h = h; r.info.scale_div = s;
    return true;
}

static bool load_stb(const uint8_t* d, size_t n, uint64_t budget, LoadResult& r) {
    int w, h, c;
    if (n > INT32_MAX || !stbi_info_from_memory(d, (int)n, &w, &h, &c)) { r.error = "unknown format"; return false; }
    stbi_uc* px = stbi_load_from_memory(d, (int)n, &w, &h, &c, 4);
    if (!px) { r.error = stbi_failure_reason(); return false; }
    int s = 1;
    while ((uint64_t)((w + s - 1) / s) * ((h + s - 1) / s) > budget) s++;
    int ow = (w + s - 1) / s, oh = (h + s - 1) / s;
    r.img = std::make_unique<Image>();
    if (!r.img->alloc(ow, oh)) { stbi_image_free(px); r.img.reset(); r.error = "out of memory"; return false; }
    std::vector<uint8_t> row((size_t)ow * 4);
    for (int y = 0; y < oh; y++) {
        const uint8_t* src = px + (size_t)y * s * w * 4;
        if (s == 1) r.img->put_rgba(y, src, ow, 4);
        else { for (int x = 0; x < ow; x++) memcpy(&row[x * 4], src + (size_t)x * s * 4, 4); r.img->put_rgba(y, row.data(), ow, 4); }
    }
    stbi_image_free(px);
    r.img->finalize(RawLayout::RGBA, c == 2 || c == 4, false);
    r.img->build_mips();
    r.info.orig_w = w; r.info.orig_h = h; r.info.scale_div = s;
    r.info.details = std::to_string(c) + " channels";
    return true;
}

// Camera RAW: show the largest embedded JPEG preview (fast, no demosaicing needed).
static bool load_raw_preview(const uint8_t* d, size_t n, uint64_t budget, LoadResult& r) {
    size_t best = 0; long best_px = 0;
    for (size_t i = 0; i + 4 < n; i++) {
        if (d[i] != 0xFF || d[i + 1] != 0xD8 || d[i + 2] != 0xFF) continue;
        // walk markers to the frame header to get the size
        size_t p = i + 2;
        while (p + 9 < n && d[p] == 0xFF) {
            uint8_t m = d[p + 1];
            size_t L = (size_t)d[p + 2] << 8 | d[p + 3];
            if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7)) {
                long hh = d[p + 5] << 8 | d[p + 6], ww = d[p + 7] << 8 | d[p + 8];
                if (ww * hh > best_px && m != 0xC3) { best_px = ww * hh; best = i; }  // C3 = lossless (raw data)
                break;
            }
            if (m == 0xDA || L < 2) break;
            p += 2 + L;
        }
    }
    if (!best_px) { r.error = "no embedded preview found in RAW file"; return false; }
    GdkPixbufLoader* ld = gdk_pixbuf_loader_new_with_type("jpeg", nullptr);
    if (!ld) { r.error = "no JPEG loader"; return false; }
    gdk_pixbuf_loader_write(ld, d + best, n - best, nullptr);
    gdk_pixbuf_loader_close(ld, nullptr);
    GdkPixbuf* pb = gdk_pixbuf_loader_get_pixbuf(ld);
    if (pb) {
        GdkPixbuf* o = gdk_pixbuf_apply_embedded_orientation(pb);
        r.img = image_from_pixbuf(o);
        g_object_unref(o);
    }
    g_object_unref(ld);
    (void)budget;
    if (!r.img) { r.error = "could not decode RAW preview"; return false; }
    r.img->build_mips();
    r.info.format = "Camera RAW";
    r.info.details = "embedded JPEG preview";
    r.info.orig_w = r.img->width(); r.info.orig_h = r.img->height();
    return true;
}

std::unique_ptr<LoadResult> load_image(const std::string& path, uint64_t budget, LoadCtl& ctl,
                                       const std::function<void(std::unique_ptr<LoadResult>)>& on_preview) {
    auto r = std::make_unique<LoadResult>();
    budget = std::max<uint64_t>(budget, 1000000);
    GError* err = nullptr;
    GMappedFile* mf = g_mapped_file_new(path.c_str(), FALSE, &err);
    if (!mf) { r->error = err ? err->message : "cannot open file"; if (err) g_error_free(err); return r; }
    const uint8_t* d = (const uint8_t*)g_mapped_file_get_contents(mf);
    size_t n = g_mapped_file_get_length(mf);
    std::string ext = lower_ext(path);
    bool ok = false;
    if (n < 8) r->error = "file too small";
    else if (!memcmp(d, "8BPS", 4)) {
        // big file: show a fast row-sampled preview (~4 MP) before the full decode
        if (on_preview && n > 8 && n >= 26) {
            uint64_t ph = (uint64_t)d[14] << 24 | d[15] << 16 | d[16] << 8 | d[17], pw = (uint64_t)d[18] << 24 | d[19] << 16 | d[20] << 8 | d[21];
            if (pw * ph > 40000000ull && budget > 4000000ull) {
                auto p = std::make_unique<LoadResult>();
                LoadCtl pc;
                if (load_psd(d, n, 4000000ull, pc, *p, true) && !ctl.cancel) on_preview(std::move(p));
            }
        }
        ok = !ctl.cancel && load_psd(d, n, budget, ctl, *r);
    }
    else if (kRaw.count(ext)) {
        ok = (ext == "dng" && load_pixbuf(path, budget, *r)) || load_raw_preview(d, n, budget, *r);
    } else if (kStb.count(ext)) {
        ok = load_stb(d, n, budget, *r) || load_pixbuf(path, budget, *r);
        if (ok && r->info.format.empty()) r->info.format = ext;
    } else {
        ok = load_pixbuf(path, budget, *r);
        if (!ok) { std::string e = r->error; ok = load_stb(d, n, budget, *r); if (!ok) r->error = e; }
    }
    if (ok) r->error.clear();
    for (auto& c : r->info.format) c = (char)g_ascii_toupper(c);
    g_mapped_file_unref(mf);
    ctl.progress = 1000;
    return r;
}
