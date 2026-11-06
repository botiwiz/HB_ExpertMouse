#include "decode.h"
#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QImageReader>
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
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

static const std::set<std::string> kRaw = {"3fr","arw","cr2","cr3","crw","dcr","dng","erf","kdc","mef","mos","mrw",
    "nef","nrw","orf","pef","raf","raw","rw2","rwl","sr2","srf","srw","x3f","iiq"};
static const std::set<std::string> kStb = {"hdr","pic","pnm","ppm","pgm","pbm","tga"};
static const std::set<std::string> k3d = {"obj","stl","ply","gltf","glb","fbx","3ds","dae","off","vtk","vtp","vtu","vtr",
    "vti","vts","vtm","step","stp","iges","igs","brep","usd","usda","usdc","usdz","abc","3mf","x3d","drc","splat","spz","pts","e57","las"};

bool is_3d_ext(const std::string& e) { return k3d.count(e) > 0; }
bool is_raw_ext(const std::string& e) { return kRaw.count(e) > 0; }

bool is_image_ext(const std::string& e) {
    static std::set<std::string> exts = [] {
        std::set<std::string> s = {"psd", "psb"};
        s.insert(kRaw.begin(), kRaw.end());
        s.insert(kStb.begin(), kStb.end());
        for (const QByteArray& f : QImageReader::supportedImageFormats()) s.insert(f.toLower().toStdString());
        s.insert("jpg"); s.insert("jpeg"); s.insert("tif"); s.insert("tiff");
        s.erase("ani"); s.erase("cur"); s.erase("pdf"); s.erase("svgz") ;  // cursors / documents: not browsed as images
        return s;
    }();
    return exts.count(e) > 0;
}

std::unique_ptr<Image> image_from_qimage(const QImage& src) {
    QImage q = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);   // = our tile layout
    auto img = std::make_unique<Image>();
    if (q.isNull() || !img->alloc(q.width(), q.height())) return nullptr;
    for (int y = 0; y < q.height(); y++) img->put_bgra(y, q.constScanLine(y), q.width());
    img->has_alpha = src.hasAlphaChannel();
    return img;
}

QImage qimage_from_image(Image* im) {
    if (!im || (uint64_t)im->width() * im->height() * 4 >= ((uint64_t)1 << 31)) return QImage();
    QImage q(im->width(), im->height(), QImage::Format_ARGB32_Premultiplied);
    if (q.isNull()) return q;
    const Level& l = im->lv[0];
    for (int y = 0; y < l.h; y++) {
        uint8_t* d = q.scanLine(y);
        for (int c = 0; c < l.cols; c++) {
            Tile* t = l.tile(c, y / Image::T);
            memcpy(d + (size_t)c * Image::T * 4, t->data + (size_t)(y % Image::T) * t->stride, (size_t)t->w * 4);
        }
    }
    return q;
}

static std::string qt_details(const QImage& q) {
    std::string s = q.hasAlphaChannel() ? "RGBA" : q.isGrayscale() ? "Grayscale" : "RGB";
    int bits = q.depth() >= 64 ? 16 : 8;
    if (q.format() == QImage::Format_RGBA32FPx4 || q.format() == QImage::Format_RGBX32FPx4) bits = 32;
    if (q.format() == QImage::Format_RGBA16FPx4 || q.format() == QImage::Format_RGBX16FPx4) bits = 16;
    return s + ", " + std::to_string(bits) + "-bit";
}

static bool load_qt(const std::string& path, uint64_t budget, LoadResult& r) {
    QImageReader rd(QString::fromStdString(path));
    rd.setAutoTransform(true);                      // EXIF orientation
    if (!rd.canRead()) { r.error = rd.errorString().toStdString(); return false; }
    r.info.format = rd.format().toUpper().toStdString();
    if (rd.supportsAnimation() && rd.imageCount() != 1) r.animated = true;
    QSize sz = rd.size();
    int s = 1;
    if (sz.isValid()) {
        while ((uint64_t)(sz.width() / s) * (sz.height() / s) > budget) s++;
        if (s > 1) rd.setScaledSize(QSize(std::max(1, sz.width() / s), std::max(1, sz.height() / s)));
    }
    rd.setAllocationLimit(0);                       // we enforce our own budget
    QImage q = rd.read();
    if (q.isNull()) { r.error = rd.errorString().toStdString(); return false; }
    r.img = image_from_qimage(q);
    if (!r.img) { r.error = "out of memory"; return false; }
    r.img->build_mips();
    r.info.details = qt_details(q);
    r.info.orig_w = sz.isValid() ? sz.width() : q.width();
    r.info.orig_h = sz.isValid() ? sz.height() : q.height();
    if (rd.transformation() & QImageIOHandler::TransformationRotate90) std::swap(r.info.orig_w, r.info.orig_h);
    r.info.scale_div = s;
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
static bool load_raw_preview(const uint8_t* d, size_t n, LoadResult& r) {
    size_t best = 0; long best_px = 0;
    for (size_t i = 0; i + 4 < n; i++) {
        if (d[i] != 0xFF || d[i + 1] != 0xD8 || d[i + 2] != 0xFF) continue;
        size_t p = i + 2;                            // walk markers to the frame header
        while (p + 9 < n && d[p] == 0xFF) {
            uint8_t m = d[p + 1];
            size_t L = (size_t)d[p + 2] << 8 | d[p + 3];
            if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7)) {
                long hh = d[p + 5] << 8 | d[p + 6], ww = d[p + 7] << 8 | d[p + 8];
                if (ww * hh > best_px && m != 0xC3) { best_px = ww * hh; best = i; }   // C3 = lossless raw data
                break;
            }
            if (m == 0xDA || L < 2) break;
            p += 2 + L;
        }
    }
    if (!best_px) { r.error = "no embedded preview found in RAW file"; return false; }
    QByteArray ba = QByteArray::fromRawData((const char*)d + best, (qsizetype)(n - best));
    QBuffer buf(&ba);
    buf.open(QIODevice::ReadOnly);
    QImageReader rd(&buf, "jpeg");
    rd.setAutoTransform(true);
    QImage q = rd.read();
    if (q.isNull()) { r.error = "could not decode RAW preview"; return false; }
    r.img = image_from_qimage(q);
    if (!r.img) { r.error = "out of memory"; return false; }
    r.img->build_mips();
    r.info.format = "Camera RAW";
    r.info.details = "embedded JPEG preview";
    r.info.orig_w = q.width(); r.info.orig_h = q.height();
    return true;
}

std::unique_ptr<LoadResult> load_image(const std::string& path, uint64_t budget, LoadCtl& ctl,
                                       const std::function<void(std::unique_ptr<LoadResult>)>& on_preview) {
    auto r = std::make_unique<LoadResult>();
    budget = std::max<uint64_t>(budget, 1000000);
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) { r->error = f.errorString().toStdString(); return r; }
    size_t n = (size_t)f.size();
    const uint8_t* d = n ? f.map(0, f.size()) : nullptr;
    if (!d) { r->error = n ? "cannot map file" : "empty file"; return r; }
    std::string ext = lower_ext(path);
    bool ok = false;
    if (n < 8) r->error = "file too small";
    else if (!memcmp(d, "8BPS", 4)) {
        // big file: show a fast row-sampled preview (~4 MP) before the full decode
        if (on_preview && n >= 26) {
            uint64_t ph = (uint64_t)d[14] << 24 | d[15] << 16 | d[16] << 8 | d[17], pw = (uint64_t)d[18] << 24 | d[19] << 16 | d[20] << 8 | d[21];
            if (pw * ph > 40000000ull && budget > 4000000ull) {
                auto p = std::make_unique<LoadResult>();
                LoadCtl pc;
                if (load_psd(d, n, 4000000ull, pc, *p, true) && !ctl.cancel) on_preview(std::move(p));
            }
        }
        ok = !ctl.cancel && load_psd(d, n, budget, ctl, *r);
    } else if (kRaw.count(ext)) {
        ok = load_raw_preview(d, n, *r) || load_qt(path, budget, *r);
    } else if (kStb.count(ext)) {
        ok = load_stb(d, n, budget, *r) || load_qt(path, budget, *r);
        if (ok && r->info.format.empty()) r->info.format = ext;
    } else {
        ok = load_qt(path, budget, *r);
        if (!ok) { std::string e = r->error; ok = load_stb(d, n, budget, *r); if (!ok) r->error = e; else r->info.format = ext; }
    }
    if (ok) r->error.clear();
    for (auto& c : r->info.format) c = (char)toupper((unsigned char)c);
    ctl.progress = 1000;
    return r;
}

std::unique_ptr<LoadResult> load_preview(const std::string& path, uint64_t max_px) {
    std::string ext = lower_ext(path);
    LoadCtl ctl;
    if (ext == "psd" || ext == "psb" || kRaw.count(ext)) {
        auto r = std::make_unique<LoadResult>();
        QFile f(QString::fromStdString(path));
        if (!f.open(QIODevice::ReadOnly) || f.size() < 26) { r->error = "cannot open"; return r; }
        const uint8_t* d = f.map(0, f.size());
        if (!d) { r->error = "cannot map file"; return r; }
        size_t n = (size_t)f.size();
        if (!memcmp(d, "8BPS", 4)) load_psd(d, n, max_px, ctl, *r, true);
        else load_raw_preview(d, n, *r);
        if (r->img) r->error.clear();
        return r;
    }
    return load_image(path, max_px, ctl);   // QImageReader uses setScaledSize (JPEG decodes scaled)
}
