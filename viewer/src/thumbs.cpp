#include "thumbs.h"
#include "decode.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>

static std::string cache_dir(const char* size) {
    return std::string(g_get_user_cache_dir()) + "/thumbnails/" + size;
}

static GdkPixbuf* scale_to_max(GdkPixbuf* pb, int max) {
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    if (w <= max && h <= max) { g_object_ref(pb); return pb; }
    double s = std::min((double)max / w, (double)max / h);
    return gdk_pixbuf_scale_simple(pb, std::max(1, (int)(w * s + 0.5)), std::max(1, (int)(h * s + 0.5)), GDK_INTERP_BILINEAR);
}

static GdkPixbuf* generate(const std::string& path) {
    std::string ext = lower_ext(path);
    GdkPixbuf* out = nullptr;
    // PSD/PSB: fast row-sampled decode straight to thumbnail size (even for multi-GB files)
    if (ext == "psd" || ext == "psb") {
        if (GMappedFile* mf = g_mapped_file_new(path.c_str(), FALSE, nullptr)) {
            LoadResult r; LoadCtl ctl;
            if (load_psd((const uint8_t*)g_mapped_file_get_contents(mf), g_mapped_file_get_length(mf),
                         (uint64_t)kThumbMax * kThumbMax * 2, ctl, r, true)) {
                if (GdkPixbuf* pb = pixbuf_from_image(r.img.get())) { out = scale_to_max(pb, kThumbMax); g_object_unref(pb); }
            }
            g_mapped_file_unref(mf);
        }
        return out;
    }
    // formats gdk-pixbuf can scale while decoding (JPEG uses fast DCT scaling)
    if (!is_raw_ext(ext) && gdk_pixbuf_get_file_info(path.c_str(), nullptr, nullptr)) {
        if (GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_size(path.c_str(), kThumbMax, kThumbMax, nullptr)) {
            out = gdk_pixbuf_apply_embedded_orientation(pb);
            g_object_unref(pb);
            return out;
        }
    }
    // everything else we decode ourselves (RAW previews, HDR, TGA...)
    LoadCtl ctl;
    auto r = load_image(path, 2000000, ctl);
    if (r->img) if (GdkPixbuf* pb = pixbuf_from_image(r->img.get())) { out = scale_to_max(pb, kThumbMax); g_object_unref(pb); }
    return out;
}

GdkPixbuf* thumb_get(const std::string& path, bool can_generate) {
    GStatBuf st;
    if (g_stat(path.c_str(), &st) != 0) return nullptr;
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (!uri) return nullptr;
    gchar* md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    std::string mtime = std::to_string((long long)st.st_mtime);
    GdkPixbuf* out = nullptr;
    for (const char* sz : {"large", "x-large", "normal"}) {
        std::string f = cache_dir(sz) + "/" + md5 + ".png";
        if (!g_file_test(f.c_str(), G_FILE_TEST_EXISTS)) continue;
        GdkPixbuf* pb = gdk_pixbuf_new_from_file(f.c_str(), nullptr);
        if (!pb) continue;
        const char* mt = gdk_pixbuf_get_option(pb, "tEXt::Thumb::MTime");
        if (mt && mtime == mt) { out = scale_to_max(pb, kThumbMax); g_object_unref(pb); break; }
        g_object_unref(pb);
    }
    std::string cache = cache_dir("large");
    bool in_cache = path.rfind(std::string(g_get_user_cache_dir()) + "/thumbnails/", 0) == 0;
    if (!out && can_generate) {
        out = generate(path);
        if (out && !in_cache && g_mkdir_with_parents(cache.c_str(), 0700) == 0) {
            std::string f = cache + "/" + md5 + ".png";
            std::string tmp = f + "." + std::to_string(getpid()) + "." + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id()) % 100000) + ".tmp";
            if (gdk_pixbuf_save(out, tmp.c_str(), "png", nullptr, "tEXt::Thumb::URI", uri, "tEXt::Thumb::MTime", mtime.c_str(),
                                "tEXt::Software", "HB Viewer", nullptr)) {
                g_chmod(tmp.c_str(), 0600);
                g_rename(tmp.c_str(), f.c_str());
            } else g_unlink(tmp.c_str());
        }
    }
    g_free(md5); g_free(uri);
    return out;
}

// ---------------------------------------------------------------- worker pool
namespace {
std::mutex mu;
std::condition_variable cv;
std::deque<std::string> queue;
std::map<std::string, bool> can_gen;        // pending paths of the current generation
int cur_gen = 0;
std::function<void(int, const std::string&, GdkPixbuf*)> on_done;

struct Result { int gen; std::string path; GdkPixbuf* pb; };

gboolean deliver(gpointer p) {
    Result* r = (Result*)p;
    on_done(r->gen, r->path, r->pb);
    if (r->pb) g_object_unref(r->pb);
    delete r;
    return G_SOURCE_REMOVE;
}

void worker() {
    for (;;) {
        std::string path; bool gen_ok; int gen;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [] { return !queue.empty(); });
            path = queue.front(); queue.pop_front();
            auto it = can_gen.find(path);
            if (it == can_gen.end()) continue;           // already done, or folder changed
            gen_ok = it->second; gen = cur_gen;
            can_gen.erase(it);
        }
        GdkPixbuf* pb = thumb_get(path, gen_ok);
        g_idle_add(deliver, new Result{gen, path, pb});
    }
}
} // namespace

void thumbs_start(std::function<void(int, const std::string&, GdkPixbuf*)> done) {
    on_done = std::move(done);
    int n = std::clamp((int)std::thread::hardware_concurrency() - 1, 2, 6);
    for (int i = 0; i < n; i++) std::thread(worker).detach();
}

void thumbs_request(int gen, const std::vector<std::pair<std::string, bool>>& items) {
    std::lock_guard<std::mutex> lk(mu);
    cur_gen = gen;
    queue.clear(); can_gen.clear();
    for (auto& it : items) { queue.push_back(it.first); can_gen[it.first] = it.second; }
    cv.notify_all();
}

void thumbs_prioritize(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lk(mu);
    for (auto it = paths.rbegin(); it != paths.rend(); ++it)
        if (can_gen.count(*it)) queue.push_front(*it);
    cv.notify_all();
}
