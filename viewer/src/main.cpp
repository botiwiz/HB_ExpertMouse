// HB Viewer - lightweight image viewer for huge PSD/PSB and common formats.
// Every function is a Command in one table: it feeds the right-click menu, the keyboard
// shortcuts and the Favorites toolbar (right-click any menu item to pin/unpin it).
#include "decode.h"
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <thread>

#define APP_NAME "HB Viewer"
#define APP_VERSION "0.1.0"

// ---------------------------------------------------------------- commands
struct Command {
    const char* id;
    const char* label;
    const char* group;
    const char* icon;                      // freedesktop icon name for the toolbar
    guint key; GdkModifierType mods;       // primary shortcut (0 = none)
    std::function<void()> run;
    std::function<bool()> checked;         // set -> shown as a check item
};
static std::vector<Command> cmds;
struct Alias { guint key; GdkModifierType mods; const char* id; };
static std::vector<Alias> aliases;
static const char* kGroups[] = {"File", "Navigate", "View", "Image", "3D", "Favorites", "Help"};

static int cmd_index(const std::string& id) {
    for (size_t i = 0; i < cmds.size(); i++) if (id == cmds[i].id) return (int)i;
    return -1;
}

// ---------------------------------------------------------------- state
enum Bg { BG_DARK, BG_GRAY, BG_WHITE, BG_CHECKER, BG_COUNT };

struct Job { std::shared_ptr<LoadCtl> ctl; };

static struct App {
    GtkWidget *win, *toolbar, *area, *status_l, *status_r, *statusbar;
    std::string ini_path;
    GKeyFile* ini;
    // files
    std::vector<std::string> files;
    int idx = -1;
    std::string want;                                   // path the user navigated to
    std::shared_ptr<LoadResult> cur;
    std::string cur_path;
    double cur_load_s = 0;
    std::map<std::string, std::shared_ptr<LoadResult>> cache;
    std::map<std::string, Job> jobs;
    std::map<std::string, std::chrono::steady_clock::time_point> job_start;
    // view
    double zoom = 1, offx = 0, offy = 0;
    int rot = 0; bool flip_h = false, flip_v = false;
    bool auto_fit = true, tiled = false;
    bool dragging = false; double drag_x = 0, drag_y = 0;
    // settings
    int bg = BG_DARK;
    bool smooth = true, wheel_zoom = true, fit_on_open = true, loop_dir = true, confirm_delete = true;
    bool show_toolbar = true, show_status = true, fullscreen = false, on_top = false;
    int slideshow_ms = 3000; guint slideshow_id = 0;
    uint64_t budget_px = 0;
    std::vector<std::string> favs;
    // animation
    GdkPixbufAnimationIter* anim_it = nullptr; guint anim_id = 0;
    guint progress_id = 0;
    std::string pixel_text;
} A;

static void update_status();
static void rebuild_toolbar();
static void show_menu(GdkEvent* ev);

// ---------------------------------------------------------------- settings
static void load_settings() {
    const char* appimage = g_getenv("APPIMAGE");
    std::string dir;
    if (appimage) {
        gchar* d = g_path_get_dirname(appimage);
        if (access(d, W_OK) == 0) dir = d;
        g_free(d);
    }
    if (dir.empty()) {
        dir = std::string(g_get_user_config_dir()) + "/hb_viewer";
        g_mkdir_with_parents(dir.c_str(), 0700);
    }
    A.ini_path = dir + "/hb_viewer.ini";
    A.ini = g_key_file_new();
    g_key_file_load_from_file(A.ini, A.ini_path.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    auto b = [](const char* k, bool def) { GError* e = nullptr; bool v = g_key_file_get_boolean(A.ini, "view", k, &e); if (e) { g_error_free(e); return def; } return v; };
    auto i = [](const char* k, int def) { GError* e = nullptr; int v = g_key_file_get_integer(A.ini, "view", k, &e); if (e) { g_error_free(e); return def; } return v; };
    A.bg = std::clamp(i("background", BG_DARK), 0, BG_COUNT - 1);
    A.smooth = b("smooth_zoom_in", true);
    A.wheel_zoom = b("wheel_zooms", true);
    A.fit_on_open = b("fit_large_images", true);
    A.loop_dir = b("loop_folder", true);
    A.confirm_delete = b("confirm_delete", true);
    A.show_toolbar = b("toolbar", true);
    A.show_status = b("statusbar", true);
    A.slideshow_ms = std::max(200, i("slideshow_ms", 3000));
    int mp = i("max_megapixels", 0);                    // 0 = auto (about a third of RAM)
    if (mp > 0) A.budget_px = (uint64_t)mp * 1000000;
    else {
        uint64_t mem = 8ull << 30;
        if (FILE* f = fopen("/proc/meminfo", "r")) { unsigned long long kb; if (fscanf(f, "MemTotal: %llu kB", &kb) == 1) mem = kb * 1024; fclose(f); }
        A.budget_px = std::clamp<uint64_t>(mem / 3 / 6, 64000000ull, 2000000000ull);   // ~5.3 B/px incl. mips
    }
    gsize n = 0;
    gchar** f = g_key_file_get_string_list(A.ini, "favorites", "commands", &n, nullptr);
    if (f) { for (gsize k = 0; k < n; k++) A.favs.push_back(f[k]); g_strfreev(f); }
    else A.favs = {"open", "prev", "next", "zoom_fit", "zoom_100", "zoom_in", "zoom_out", "rotate_left", "rotate_right", "fullscreen", "info"};
}

static void save_settings() {
    auto sb = [](const char* k, bool v) { g_key_file_set_boolean(A.ini, "view", k, v); };
    g_key_file_set_integer(A.ini, "view", "background", A.bg);
    sb("smooth_zoom_in", A.smooth); sb("wheel_zooms", A.wheel_zoom); sb("fit_large_images", A.fit_on_open);
    sb("loop_folder", A.loop_dir); sb("confirm_delete", A.confirm_delete);
    sb("toolbar", A.show_toolbar); sb("statusbar", A.show_status);
    g_key_file_set_integer(A.ini, "view", "slideshow_ms", A.slideshow_ms);
    if (!g_key_file_has_key(A.ini, "view", "max_megapixels", nullptr)) {
        g_key_file_set_integer(A.ini, "view", "max_megapixels", 0);
        g_key_file_set_comment(A.ini, "view", "max_megapixels", " Larger images are decoded downscaled. 0 = auto (about 1/3 of RAM)", nullptr);
    }
    std::vector<const gchar*> v;
    for (auto& s : A.favs) v.push_back(s.c_str());
    g_key_file_set_string_list(A.ini, "favorites", "commands", v.data(), v.size());
    if (!A.fullscreen && A.win && gtk_widget_get_realized(A.win)) {
        int w, h; gtk_window_get_size(GTK_WINDOW(A.win), &w, &h);
        g_key_file_set_integer(A.ini, "window", "width", w);
        g_key_file_set_integer(A.ini, "window", "height", h);
        g_key_file_set_boolean(A.ini, "window", "maximized", gtk_window_is_maximized(GTK_WINDOW(A.win)));
    }
    g_key_file_save_to_file(A.ini, A.ini_path.c_str(), nullptr);
}

// ---------------------------------------------------------------- view math
static Image* img() { return A.cur && A.cur->img ? A.cur->img.get() : nullptr; }
static int area_w() { return gtk_widget_get_allocated_width(A.area); }
static int area_h() { return gtk_widget_get_allocated_height(A.area); }

// image (level-0 pixel) coordinates -> widget coordinates
static void view_matrix(cairo_matrix_t* m) {
    Image* im = img();
    cairo_matrix_init_translate(m, area_w() / 2.0 + A.offx, area_h() / 2.0 + A.offy);
    cairo_matrix_rotate(m, A.rot * M_PI / 2);
    cairo_matrix_scale(m, A.zoom * (A.flip_h ? -1 : 1), A.zoom * (A.flip_v ? -1 : 1));
    if (im) cairo_matrix_translate(m, -im->width() / 2.0, -im->height() / 2.0);
}

static void rotated_size(double& w, double& h) {
    Image* im = img();
    w = im ? im->width() : 1; h = im ? im->height() : 1;
    if (A.rot & 1) std::swap(w, h);
}

static void fit(bool always) {
    if (!img()) return;
    double w, h; rotated_size(w, h);
    double z = std::min(area_w() / w, area_h() / h);
    A.zoom = always ? z : std::min(1.0, z);
    A.offx = A.offy = 0; A.auto_fit = true;
    gtk_widget_queue_draw(A.area);
    update_status();
}

static void set_zoom(double z, double sx, double sy) {
    z = std::clamp(z, 0.0005, 256.0);
    double cx = sx - area_w() / 2.0, cy = sy - area_h() / 2.0;
    A.offx = cx + (A.offx - cx) * z / A.zoom;
    A.offy = cy + (A.offy - cy) * z / A.zoom;
    A.zoom = z; A.auto_fit = false;
    gtk_widget_queue_draw(A.area);
    update_status();
}

static void zoom_step(int dir, double sx, double sy) {
    static const double steps[] = {0.01, 0.02, 0.03, 0.05, 0.0625, 0.083, 0.1, 0.125, 0.167, 0.25, 0.333, 0.5, 0.667, 0.75,
                                   1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
    double z = A.zoom;
    if (dir > 0) { for (double s : steps) if (s > z * 1.001) { set_zoom(s, sx, sy); return; } set_zoom(z * 1.5, sx, sy); }
    else { for (int i = (int)G_N_ELEMENTS(steps) - 1; i >= 0; i--) if (steps[i] < z / 1.001) { set_zoom(steps[i], sx, sy); return; } set_zoom(z / 1.5, sx, sy); }
}

// ---------------------------------------------------------------- drawing
static void draw_checker(cairo_t* cr) {
    static cairo_pattern_t* pat = nullptr;
    if (!pat) {
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 16, 16);
        cairo_t* c = cairo_create(s);
        cairo_set_source_rgb(c, 0.8, 0.8, 0.8); cairo_paint(c);
        cairo_set_source_rgb(c, 0.6, 0.6, 0.6);
        cairo_rectangle(c, 0, 0, 8, 8); cairo_rectangle(c, 8, 8, 8, 8); cairo_fill(c);
        cairo_destroy(c);
        pat = cairo_pattern_create_for_surface(s);
        cairo_surface_destroy(s);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    }
    cairo_set_source(cr, pat);
}

static void draw_image_once(cairo_t* cr, Image* im, double dx, double dy, double W, double H) {
    cairo_matrix_t m; view_matrix(&m);
    cairo_matrix_translate(&m, dx, dy);
    // pick the mip level whose resolution is just above what the screen needs
    int L = 0;
    while (L + 1 < (int)im->lv.size() && im->lv[L + 1].w >= A.zoom * im->width()) L++;
    const Level& l = im->lv[L];
    double fx = (double)im->width() / l.w, fy = (double)im->height() / l.h;
    cairo_matrix_t lm = m;
    cairo_matrix_scale(&lm, fx, fy);                     // level pixels -> screen

    // visible area in level coordinates
    cairo_matrix_t inv = lm;
    if (cairo_matrix_invert(&inv) != CAIRO_STATUS_SUCCESS) return;
    double xs[4] = {0, W, 0, W}, ys[4] = {0, 0, H, H}, x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (int k = 0; k < 4; k++) {
        double x = xs[k], y = ys[k]; cairo_matrix_transform_point(&inv, &x, &y);
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    int c0 = std::max(0, (int)floor(x0 / Image::T)), c1 = std::min(l.cols - 1, (int)floor(x1 / Image::T));
    int r0 = std::max(0, (int)floor(y0 / Image::T)), r1 = std::min(l.rows - 1, (int)floor(y1 / Image::T));
    if (c0 > c1 || r0 > r1) return;

    double scr = A.zoom * fx;                            // screen px per level px
    cairo_filter_t filt = scr >= 1.0 ? (A.smooth ? CAIRO_FILTER_BILINEAR : CAIRO_FILTER_NEAREST)
                                     : CAIRO_FILTER_GOOD;
    if (im->has_alpha) {                                 // checkerboard behind transparent pixels
        cairo_save(cr);
        cairo_set_matrix(cr, &lm);
        cairo_rectangle(cr, 0, 0, l.w, l.h);
        cairo_identity_matrix(cr);
        draw_checker(cr);
        cairo_fill(cr);
        cairo_restore(cr);
    }
    cairo_save(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);       // tiles butt together without seams
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            cairo_surface_t* s = l.tile(c, r);
            cairo_matrix_t tm = lm;
            cairo_matrix_translate(&tm, c * Image::T, r * Image::T);
            cairo_set_matrix(cr, &tm);
            cairo_set_source_surface(cr, s, 0, 0);
            cairo_pattern_t* p = cairo_get_source(cr);
            cairo_pattern_set_filter(p, filt);
            cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
            cairo_rectangle(cr, 0, 0, cairo_image_surface_get_width(s), cairo_image_surface_get_height(s));
            cairo_fill(cr);
        }
    cairo_restore(cr);
}

static gboolean on_draw(GtkWidget*, cairo_t* cr, gpointer) {
    double W = area_w(), H = area_h();
    switch (A.bg) {
    case BG_DARK: cairo_set_source_rgb(cr, 0.11, 0.11, 0.12); break;
    case BG_GRAY: cairo_set_source_rgb(cr, 0.5, 0.5, 0.5); break;
    case BG_WHITE: cairo_set_source_rgb(cr, 1, 1, 1); break;
    default: draw_checker(cr);
    }
    cairo_paint(cr);
    Image* im = img();
    if (!im) {
        std::string msg;
        std::string ext = lower_ext(A.want);
        if (!A.want.empty() && is_3d_ext(ext))
            msg = "3D model: " + std::string(g_path_get_basename(A.want.c_str())) + "\nPress D to open it in f3d.\n(Embedded 3D view is the next step.)";
        else if (A.cur && !A.cur->error.empty()) msg = "Cannot open this file:\n" + A.cur->error;
        else if (!A.want.empty()) msg = "Loading...";
        else msg = "Open an image with Ctrl+O, or drop a file here.\nRight-click anywhere for all commands.";
        PangoLayout* pl = pango_cairo_create_layout(cr);
        pango_layout_set_text(pl, msg.c_str(), -1);
        pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
        PangoFontDescription* fd = pango_font_description_from_string("Sans 12");
        pango_layout_set_font_description(pl, fd);
        pango_font_description_free(fd);
        int tw, th; pango_layout_get_pixel_size(pl, &tw, &th);
        bool light = A.bg == BG_WHITE || A.bg == BG_CHECKER;
        cairo_set_source_rgb(cr, light ? 0.2 : 0.75, light ? 0.2 : 0.75, light ? 0.2 : 0.75);
        cairo_move_to(cr, (W - tw) / 2, (H - th) / 2);
        pango_cairo_show_layout(cr, pl);
        g_object_unref(pl);
        return TRUE;
    }
    if (A.tiled) {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) draw_image_once(cr, im, dx * im->width(), dy * im->height(), W, H);
    } else draw_image_once(cr, im, 0, 0, W, H);
    return TRUE;
}

// ---------------------------------------------------------------- files & loading
static std::string basename_of(const std::string& p) { gchar* b = g_path_get_basename(p.c_str()); std::string s = b; g_free(b); return s; }

static void scan_folder(const std::string& path) {
    gchar* dir = g_path_get_dirname(path.c_str());
    A.files.clear();
    if (GDir* d = g_dir_open(dir, 0, nullptr)) {
        std::vector<std::pair<std::string, std::string>> v;
        while (const char* n = g_dir_read_name(d)) {
            std::string e = lower_ext(n);
            if (!is_image_ext(e) && !is_3d_ext(e)) continue;
            gchar* key = g_utf8_collate_key_for_filename(n, -1);
            v.emplace_back(key, std::string(dir) + "/" + n);
            g_free(key);
        }
        g_dir_close(d);
        std::sort(v.begin(), v.end());
        for (auto& p : v) A.files.push_back(p.second);
    }
    g_free(dir);
    A.idx = -1;
    for (size_t i = 0; i < A.files.size(); i++) if (A.files[i] == path) A.idx = (int)i;
    if (A.idx < 0) { A.files.insert(A.files.begin(), path); A.idx = 0; }
}

struct Done { std::string path; std::unique_ptr<LoadResult> r; };

static void stop_animation() {
    if (A.anim_id) { g_source_remove(A.anim_id); A.anim_id = 0; }
    if (A.anim_it) { g_object_unref(A.anim_it); A.anim_it = nullptr; }
}

static gboolean anim_tick(gpointer) {
    A.anim_id = 0;
    if (!A.cur || !A.cur->anim || !A.anim_it) return G_SOURCE_REMOVE;
    gdk_pixbuf_animation_iter_advance(A.anim_it, nullptr);
    GdkPixbuf* pb = gdk_pixbuf_animation_iter_get_pixbuf(A.anim_it);
    if (auto im = image_from_pixbuf(pb)) { im->build_mips(); A.cur->img = std::move(im); }
    gtk_widget_queue_draw(A.area);
    int d = gdk_pixbuf_animation_iter_get_delay_time(A.anim_it);
    if (d >= 0) A.anim_id = g_timeout_add(std::max(d, 20), anim_tick, nullptr);
    return G_SOURCE_REMOVE;
}

static void start_job(const std::string& path);

static void preload_neighbours(int dir) {
    // keep only current + neighbours cached
    std::vector<std::string> keep = {A.cur_path};
    if (A.files.size() > 1) {
        int n = (int)A.files.size();
        keep.push_back(A.files[(A.idx + 1) % n]);
        keep.push_back(A.files[(A.idx + n - 1) % n]);
    }
    for (auto it = A.cache.begin(); it != A.cache.end();)
        if (std::find(keep.begin(), keep.end(), it->first) == keep.end()) it = A.cache.erase(it); else ++it;
    if (A.files.size() < 2 || !img() || img()->bytes() > (400u << 20)) return;
    int n = (int)A.files.size();
    const std::string& p = A.files[(A.idx + (dir < 0 ? n - 1 : 1)) % n];
    GStatBuf st;
    if (is_3d_ext(lower_ext(p)) || A.cache.count(p) || A.jobs.count(p) || g_stat(p.c_str(), &st) != 0 || st.st_size > (200 << 20)) return;
    start_job(p);
}

static int last_dir = 1;

static void show_result(const std::string& path, std::shared_ptr<LoadResult> r) {
    stop_animation();
    // full-res result replacing its own quick preview: keep the user's zoom/pan/rotation
    bool upgrade = A.cur && A.cur->info.preview && A.cur_path == path && A.cur->img && r->img;
    double old_w = upgrade ? A.cur->img->width() : 0;
    A.cur = std::move(r);
    A.cur_path = path;
    if (upgrade) {
        if (A.auto_fit) fit(false);
        else A.zoom *= old_w / A.cur->img->width();
    } else {
        A.rot = 0; A.flip_h = A.flip_v = false;
        if (img()) { if (A.fit_on_open) fit(false); else { A.zoom = 1; A.offx = A.offy = 0; A.auto_fit = false; } }
    }
    if (A.cur->anim) {
        A.anim_it = gdk_pixbuf_animation_get_iter(A.cur->anim, nullptr);
        int d = gdk_pixbuf_animation_iter_get_delay_time(A.anim_it);
        if (d >= 0) A.anim_id = g_timeout_add(std::max(d, 20), anim_tick, nullptr);
    }
    gchar* t = g_strdup_printf("%s - " APP_NAME, basename_of(path).c_str());
    gtk_window_set_title(GTK_WINDOW(A.win), t);
    g_free(t);
    gtk_widget_queue_draw(A.area);
    update_status();
    if (!A.cur->info.preview) preload_neighbours(last_dir);
}

static gboolean job_preview(gpointer p) {
    std::unique_ptr<Done> d((Done*)p);
    auto it = A.jobs.find(d->path);
    if (it == A.jobs.end() || it->second.ctl->cancel || d->path != A.want) return G_SOURCE_REMOVE;
    if (A.cur_path == d->path && A.cur && !A.cur->info.preview) return G_SOURCE_REMOVE;
    show_result(d->path, std::shared_ptr<LoadResult>(std::move(d->r)));
    return G_SOURCE_REMOVE;
}

static gboolean job_done(gpointer p) {
    std::unique_ptr<Done> d((Done*)p);
    auto it = A.jobs.find(d->path);
    double secs = 0;
    if (A.job_start.count(d->path))
        secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - A.job_start[d->path]).count();
    if (it != A.jobs.end()) { bool cancelled = it->second.ctl->cancel; A.jobs.erase(it); if (cancelled) return G_SOURCE_REMOVE; }
    std::shared_ptr<LoadResult> r(std::move(d->r));
    if (r->img) A.cache[d->path] = r;
    if (d->path == A.want) { A.cur_load_s = secs; show_result(d->path, r); }
    return G_SOURCE_REMOVE;
}

static void start_job(const std::string& path) {
    auto ctl = std::make_shared<LoadCtl>();
    A.jobs[path] = {ctl};
    A.job_start[path] = std::chrono::steady_clock::now();
    uint64_t budget = A.budget_px;
    std::thread([path, ctl, budget] {
        auto r = load_image(path, budget, *ctl, [&path](std::unique_ptr<LoadResult> p) {
            g_idle_add(job_preview, new Done{path, std::move(p)}); });
        g_idle_add(job_done, new Done{path, std::move(r)});
    }).detach();
}

static gboolean progress_tick(gpointer) {
    if (A.jobs.count(A.want)) { update_status(); return G_SOURCE_CONTINUE; }
    A.progress_id = 0;
    return G_SOURCE_REMOVE;
}

static void open_path(const std::string& path) {
    A.want = path;
    for (auto& j : A.jobs) if (j.first != path) j.second.ctl->cancel = true;  // drop stale work
    if (is_3d_ext(lower_ext(path))) {
        stop_animation(); A.cur.reset(); A.cur_path = path;
        gtk_window_set_title(GTK_WINDOW(A.win), (basename_of(path) + " - " APP_NAME).c_str());
        gtk_widget_queue_draw(A.area); update_status();
        return;
    }
    auto c = A.cache.find(path);
    if (c != A.cache.end()) { A.cur_load_s = 0; show_result(path, c->second); return; }
    if (!A.jobs.count(path)) start_job(path);
    if (!A.progress_id) A.progress_id = g_timeout_add(100, progress_tick, nullptr);
    update_status();
}

static void open_file(const std::string& path) {
    gchar* abs = g_canonicalize_filename(path.c_str(), nullptr);
    std::string p = abs; g_free(abs);
    if (g_file_test(p.c_str(), G_FILE_TEST_IS_DIR)) {
        scan_folder(p + "/.");
        A.files.erase(A.files.begin() + A.idx);           // the dummy entry
        if (A.files.empty()) { A.idx = -1; return; }
        A.idx = 0;
    } else scan_folder(p);
    A.cache.clear();
    open_path(A.files[A.idx]);
}

static void go(int delta, bool absolute = false) {
    if (A.files.empty()) return;
    int n = (int)A.files.size(), i;
    if (absolute) i = delta < 0 ? n - 1 : 0;
    else {
        i = A.idx + delta;
        if (i < 0 || i >= n) { if (!A.loop_dir) return; i = (i % n + n) % n; }
    }
    last_dir = delta < 0 ? -1 : 1;
    A.idx = i;
    open_path(A.files[i]);
}

// ---------------------------------------------------------------- status bar
static std::string human_size(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"}; int k = 0;
    while (b >= 1024 && k < 4) { b /= 1024; k++; }
    char s[32]; snprintf(s, sizeof s, k ? "%.1f %s" : "%.0f %s", b, u[k]); return s;
}

static void update_status() {
    std::string l, r;
    if (!A.files.empty() && A.idx >= 0) l = std::to_string(A.idx + 1) + "/" + std::to_string(A.files.size()) + "   " + basename_of(A.want);
    if (A.jobs.count(A.want) && (A.cur_path != A.want || (A.cur && A.cur->info.preview))) {
        l += "   loading" + std::string(A.cur_path == A.want ? " full resolution " : " ") + std::to_string(A.jobs[A.want].ctl->progress / 10) + "%";
        if (A.cur_path == A.want && A.cur->info.orig_w) l += "   " + std::to_string(A.cur->info.orig_w) + " x " + std::to_string(A.cur->info.orig_h) + "  " + A.cur->info.format;
    } else if (A.cur && A.cur_path == A.want) {
        const ImageInfo& in = A.cur->info;
        if (img()) {
            l += "   " + std::to_string(in.orig_w) + " x " + std::to_string(in.orig_h);
            if (in.scale_div > 1) l += " (shown at 1/" + std::to_string(in.scale_div) + ")";
            l += "   " + in.format;
            if (!in.details.empty()) l += " " + in.details;
        }
        GStatBuf st;
        if (g_stat(A.want.c_str(), &st) == 0) l += "   " + human_size((double)st.st_size);
        if (A.cur_load_s > 0) { char b[32]; snprintf(b, sizeof b, "   %.2f s", A.cur_load_s); l += b; }
    }
    if (img()) {
        char b[64];
        double z = A.zoom / (A.cur->info.scale_div > 0 ? A.cur->info.scale_div : 1);
        snprintf(b, sizeof b, "%s%.1f%%", A.pixel_text.empty() ? "" : "   ", z * 100);
        r = A.pixel_text + b;
        if (A.slideshow_id) r = "Slideshow   " + r;
    }
    gtk_label_set_text(GTK_LABEL(A.status_l), l.c_str());
    gtk_label_set_text(GTK_LABEL(A.status_r), r.c_str());
}

// ---------------------------------------------------------------- helpers for commands
static GdkPixbuf* pixbuf_from_image(Image* im) {   // straight alpha
    if (!im || (uint64_t)im->width() * im->height() * 4 >= INT32_MAX) return nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, im->width(), im->height());
    if (!pb) return nullptr;
    guint8* px = gdk_pixbuf_get_pixels(pb); int rs = gdk_pixbuf_get_rowstride(pb);
    for (int y = 0; y < im->height(); y++)
        for (int x = 0; x < im->width(); x++) im->pixel(x, y, px + (size_t)y * rs + x * 4);
    return pb;
}

static GdkPixbuf* current_pixbuf() {          // incl. rotation/flip
    GdkPixbuf* pb = pixbuf_from_image(img());
    if (!pb) return nullptr;
    GdkPixbuf* t = pb;
    if (A.flip_h) { t = gdk_pixbuf_flip(pb, TRUE); g_object_unref(pb); pb = t; }
    if (A.flip_v) { t = gdk_pixbuf_flip(pb, FALSE); g_object_unref(pb); pb = t; }
    if (A.rot) { t = gdk_pixbuf_rotate_simple(pb, A.rot == 1 ? GDK_PIXBUF_ROTATE_CLOCKWISE : A.rot == 2 ? GDK_PIXBUF_ROTATE_UPSIDEDOWN : GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE); g_object_unref(pb); pb = t; }
    return pb;
}

static void message(GtkMessageType t, const std::string& s) {
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(A.win), GTK_DIALOG_MODAL, t, GTK_BUTTONS_OK, "%s", s.c_str());
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
}

static std::string find_f3d() {
    if (const char* ad = g_getenv("APPDIR")) {
        std::string p = std::string(ad) + "/usr/bin/f3d";
        if (g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE)) return p;
    }
    gchar* p = g_find_program_in_path("f3d");
    std::string s = p ? p : ""; g_free(p); return s;
}

static void cmd_open() {
    GtkFileChooserNative* fc = gtk_file_chooser_native_new("Open", GTK_WINDOW(A.win), GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
    if (!A.cur_path.empty()) { gchar* d = g_path_get_dirname(A.cur_path.c_str()); gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), d); g_free(d); }
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        gchar* f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if (f) { open_file(f); g_free(f); }
    }
    g_object_unref(fc);
}

static void cmd_save_as() {
    GdkPixbuf* pb = current_pixbuf();
    if (!pb) { message(GTK_MESSAGE_WARNING, img() ? "Image is too large to save (over 2 GB of pixels)." : "No image."); return; }
    GtkFileChooserNative* fc = gtk_file_chooser_native_new("Save As (png, jpg, bmp, tiff, ico)", GTK_WINDOW(A.win), GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
    std::string base = basename_of(A.cur_path);
    base = base.substr(0, base.find_last_of('.')) + ".png";
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), base.c_str());
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        gchar* f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        std::string e = lower_ext(f);
        const char* type = e == "jpg" || e == "jpeg" ? "jpeg" : e == "bmp" ? "bmp" : e == "tif" || e == "tiff" ? "tiff" : e == "ico" ? "ico" : "png";
        GdkPixbuf* out = pb; g_object_ref(out);
        if (!strcmp(type, "jpeg") || !strcmp(type, "bmp")) {     // no alpha: flatten on white
            g_object_unref(out);
            out = gdk_pixbuf_composite_color_simple(pb, gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb), GDK_INTERP_NEAREST, 255, 8, 0xffffff, 0xffffff);
            GdkPixbuf* rgb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb));
            gdk_pixbuf_copy_area(out, 0, 0, gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb), rgb, 0, 0); // drops alpha
            g_object_unref(out); out = rgb;
        }
        GError* err = nullptr;
        gboolean ok = !strcmp(type, "jpeg") ? gdk_pixbuf_save(out, f, type, &err, "quality", "92", nullptr)
                                            : gdk_pixbuf_save(out, f, type, &err, nullptr);
        if (!ok) { message(GTK_MESSAGE_ERROR, err ? err->message : "Save failed"); if (err) g_error_free(err); }
        g_object_unref(out);
        g_free(f);
    }
    g_object_unref(fc);
    g_object_unref(pb);
}

static void cmd_rename() {
    if (A.cur_path.empty()) return;
    GtkWidget* d = gtk_dialog_new_with_buttons("Rename", GTK_WINDOW(A.win), GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_Rename", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget* e = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    std::string base = basename_of(A.cur_path);
    gtk_entry_set_text(GTK_ENTRY(e), base.c_str());
    gtk_widget_set_size_request(e, 420, -1);
    gtk_container_set_border_width(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), 10);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), e);
    gtk_widget_show_all(d);
    auto dot = base.find_last_of('.');
    gtk_editable_select_region(GTK_EDITABLE(e), 0, dot == std::string::npos ? -1 : (int)g_utf8_strlen(base.c_str(), dot));
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        std::string nn = gtk_entry_get_text(GTK_ENTRY(e));
        if (!nn.empty() && nn != base) {
            GFile* f = g_file_new_for_path(A.cur_path.c_str());
            GError* err = nullptr;
            GFile* nf = g_file_set_display_name(f, nn.c_str(), nullptr, &err);
            if (nf) {
                gchar* np = g_file_get_path(nf);
                auto c = A.cache.find(A.cur_path);
                if (c != A.cache.end()) { A.cache[np] = c->second; A.cache.erase(A.cur_path); }
                A.files[A.idx] = np; A.cur_path = A.want = np;
                gtk_window_set_title(GTK_WINDOW(A.win), (basename_of(np) + " - " APP_NAME).c_str());
                g_free(np); g_object_unref(nf);
                update_status();
            } else { message(GTK_MESSAGE_ERROR, err->message); g_error_free(err); }
            g_object_unref(f);
        }
    }
    gtk_widget_destroy(d);
}

static void cmd_trash() {
    if (A.files.empty() || A.idx < 0) return;
    std::string p = A.files[A.idx];
    if (A.confirm_delete) {
        GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(A.win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
                                              "Move \"%s\" to the trash?", basename_of(p).c_str());
        int resp = gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
        if (resp != GTK_RESPONSE_OK) return;
    }
    GFile* f = g_file_new_for_path(p.c_str());
    GError* err = nullptr;
    if (!g_file_trash(f, nullptr, &err)) { message(GTK_MESSAGE_ERROR, err->message); g_error_free(err); g_object_unref(f); return; }
    g_object_unref(f);
    A.cache.erase(p);
    A.files.erase(A.files.begin() + A.idx);
    if (A.files.empty()) { A.idx = -1; A.want.clear(); A.cur.reset(); A.cur_path.clear(); stop_animation(); gtk_widget_queue_draw(A.area); update_status(); return; }
    if (A.idx >= (int)A.files.size()) A.idx = (int)A.files.size() - 1;
    open_path(A.files[A.idx]);
}

static void cmd_info() {
    if (A.want.empty()) return;
    std::string s = "File: " + A.want + "\n";
    GStatBuf st;
    if (g_stat(A.want.c_str(), &st) == 0) {
        GDateTime* dt = g_date_time_new_from_unix_local(st.st_mtime);
        gchar* ds = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
        s += "Size: " + human_size((double)st.st_size) + " (" + std::to_string((long long)st.st_size) + " bytes)\nModified: " + ds + "\n";
        g_free(ds); g_date_time_unref(dt);
    }
    if (A.cur && A.cur_path == A.want) {
        const ImageInfo& in = A.cur->info;
        if (img()) {
            s += "Format: " + in.format + "\n";
            if (!in.details.empty()) s += "Details: " + in.details + "\n";
            s += "Dimensions: " + std::to_string(in.orig_w) + " x " + std::to_string(in.orig_h) + " (" +
                 std::to_string((long long)in.orig_w * in.orig_h / 100000 / 10.0).substr(0, 6) + " MP)\n";
            if (in.scale_div > 1) s += "Shown at 1/" + std::to_string(in.scale_div) + " (" + std::to_string(img()->width()) + " x " +
                                       std::to_string(img()->height()) + ") to stay within the memory budget\n";
            s += "Memory used: " + human_size((double)img()->bytes()) + " (" + std::to_string(img()->lv.size()) + " mip levels)\n";
            s += "Transparency: " + std::string(img()->has_alpha ? "yes" : "no") + "\n";
            if (A.cur_load_s > 0) { char b[32]; snprintf(b, sizeof b, "%.3f s", A.cur_load_s); s += std::string("Load time: ") + b + "\n"; }
        } else s += "Error: " + A.cur->error + "\n";
    }
    char b[64]; snprintf(b, sizeof b, "\nDecode budget: %.0f MP", A.budget_px / 1e6);
    s += b;
    message(GTK_MESSAGE_INFO, s);
}

static void cmd_open_with() {
    if (A.cur_path.empty()) return;
    GFile* f = g_file_new_for_path(A.cur_path.c_str());
    GtkWidget* d = gtk_app_chooser_dialog_new(GTK_WINDOW(A.win), GTK_DIALOG_MODAL, f);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        GAppInfo* ai = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(d));
        if (ai) {
            GList* l = g_list_append(nullptr, f);
            g_app_info_launch(ai, l, nullptr, nullptr);
            g_list_free(l); g_object_unref(ai);
        }
    }
    gtk_widget_destroy(d);
    g_object_unref(f);
}

static void cmd_show_in_folder() {
    if (A.cur_path.empty()) return;
    gchar* uri = g_filename_to_uri(A.cur_path.c_str(), nullptr, nullptr);
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    bool ok = false;
    if (bus) {
        const gchar* uris[] = {uri, nullptr};
        GVariant* res = g_dbus_connection_call_sync(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1", "ShowItems", g_variant_new("(^ass)", uris, ""), nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
        if (res) { ok = true; g_variant_unref(res); }
        g_object_unref(bus);
    }
    if (!ok) {
        gchar* dir = g_path_get_dirname(A.cur_path.c_str());
        gchar* du = g_filename_to_uri(dir, nullptr, nullptr);
        g_app_info_launch_default_for_uri(du, nullptr, nullptr);
        g_free(du); g_free(dir);
    }
    g_free(uri);
}

static void cmd_f3d() {
    if (A.want.empty()) return;
    std::string f3d = find_f3d();
    if (f3d.empty()) { message(GTK_MESSAGE_INFO, "f3d was not found.\nInstall it (https://f3d.app) or put it in your PATH."); return; }
    const gchar* argv[] = {f3d.c_str(), A.want.c_str(), nullptr};
    GError* err = nullptr;
    if (!g_spawn_async(nullptr, (gchar**)argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &err)) {
        message(GTK_MESSAGE_ERROR, err->message); g_error_free(err);
    }
}

static void toggle_fullscreen() {
    A.fullscreen = !A.fullscreen;
    if (A.fullscreen) gtk_window_fullscreen(GTK_WINDOW(A.win)); else gtk_window_unfullscreen(GTK_WINDOW(A.win));
    gtk_widget_set_visible(A.toolbar, A.show_toolbar && !A.fullscreen);
    gtk_widget_set_visible(A.statusbar, A.show_status && !A.fullscreen);
}

static gboolean slideshow_tick(gpointer) { go(1); return G_SOURCE_CONTINUE; }

static void transform_changed() { if (A.auto_fit) fit(false); else gtk_widget_queue_draw(A.area); update_status(); }

static void customize_favorites();

static void shortcuts_help() {
    std::string s;
    for (const char* g : kGroups) {
        s += std::string("\n") + g + "\n";
        for (auto& c : cmds) {
            if (strcmp(c.group, g)) continue;
            std::string keys;
            if (c.key) { gchar* a = gtk_accelerator_get_label(c.key, c.mods); keys = a; g_free(a); }
            for (auto& al : aliases) if (!strcmp(al.id, c.id)) { gchar* a = gtk_accelerator_get_label(al.key, al.mods); keys += (keys.empty() ? "" : ", ") + std::string(a); g_free(a); }
            s += "   " + std::string(c.label) + (keys.empty() ? "" : "   [" + keys + "]") + "\n";
        }
    }
    s += "\nMouse: drag = pan, wheel = zoom (or navigate, see View), double-click = full screen,\n"
         "back/forward buttons = previous/next, right-click = this command menu.\n"
         "Right-click a command in the menu to add it to / remove it from the Favorites toolbar.";
    GtkWidget* d = gtk_dialog_new_with_buttons("Commands and shortcuts", GTK_WINDOW(A.win), GTK_DIALOG_MODAL, "_Close", GTK_RESPONSE_CLOSE, nullptr);
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(sw, 560, 520);
    GtkWidget* lab = gtk_label_new(s.c_str());
    gtk_label_set_selectable(GTK_LABEL(lab), TRUE);
    gtk_widget_set_halign(lab, GTK_ALIGN_START);
    gtk_widget_set_margin_start(lab, 12);
    gtk_container_add(GTK_CONTAINER(sw), lab);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), sw, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

#define K(k) GDK_KEY_##k
#define CTRL GDK_CONTROL_MASK
#define SHIFT GDK_SHIFT_MASK
#define NOMOD ((GdkModifierType)0)

static void register_commands() {
    auto add = [](const char* id, const char* label, const char* group, const char* icon, guint key, GdkModifierType mods,
                  std::function<void()> run, std::function<bool()> checked = nullptr) {
        cmds.push_back({id, label, group, icon, key, mods, std::move(run), std::move(checked)});
    };
    auto C = [](int a, int b) { return (GdkModifierType)(a | b); };
    // File
    add("open", "Open...", "File", "document-open", K(o), CTRL, cmd_open);
    add("reload", "Reload", "File", "view-refresh", K(F5), NOMOD, [] { if (!A.want.empty()) { A.cache.erase(A.want); A.cur.reset(); A.cur_path.clear(); open_path(A.want); } });
    add("save_as", "Save as...", "File", "document-save-as", K(s), CTRL, cmd_save_as);
    add("open_with", "Open with...", "File", "system-run", K(e), NOMOD, cmd_open_with);
    add("show_folder", "Show in file manager", "File", "folder-open", K(o), C(CTRL, SHIFT), cmd_show_in_folder);
    add("copy_image", "Copy image", "File", "edit-copy", K(c), CTRL, [] {
        if (GdkPixbuf* pb = current_pixbuf()) { gtk_clipboard_set_image(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), pb); g_object_unref(pb); } });
    add("copy_path", "Copy file path", "File", "edit-paste", K(c), C(CTRL, SHIFT), [] {
        if (!A.want.empty()) gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), A.want.c_str(), -1); });
    add("rename", "Rename...", "File", "document-edit", K(F2), NOMOD, cmd_rename);
    add("trash", "Move to trash", "File", "user-trash", K(Delete), NOMOD, cmd_trash);
    add("info", "File info", "File", "dialog-information", K(i), NOMOD, cmd_info);
    add("quit", "Quit", "File", "application-exit", K(q), CTRL, [] { gtk_widget_destroy(A.win); });
    // Navigate
    add("next", "Next image", "Navigate", "go-next", K(Right), NOMOD, [] { go(1); });
    add("prev", "Previous image", "Navigate", "go-previous", K(Left), NOMOD, [] { go(-1); });
    add("first", "First image", "Navigate", "go-first", K(Home), NOMOD, [] { go(1, true); });
    add("last", "Last image", "Navigate", "go-last", K(End), NOMOD, [] { go(-1, true); });
    add("slideshow", "Slideshow", "Navigate", "media-playback-start", K(s), NOMOD, [] {
        if (A.slideshow_id) { g_source_remove(A.slideshow_id); A.slideshow_id = 0; }
        else A.slideshow_id = g_timeout_add(A.slideshow_ms, slideshow_tick, nullptr);
        update_status(); }, [] { return A.slideshow_id != 0; });
    add("loop", "Loop at end of folder", "Navigate", "media-playlist-repeat", 0, NOMOD, [] { A.loop_dir = !A.loop_dir; }, [] { return A.loop_dir; });
    // View
    add("zoom_in", "Zoom in", "View", "zoom-in", K(plus), NOMOD, [] { zoom_step(1, area_w() / 2.0, area_h() / 2.0); });
    add("zoom_out", "Zoom out", "View", "zoom-out", K(minus), NOMOD, [] { zoom_step(-1, area_w() / 2.0, area_h() / 2.0); });
    add("zoom_100", "Actual pixels (100%)", "View", "zoom-original", K(1), NOMOD, [] {
        int d = A.cur ? A.cur->info.scale_div : 1; set_zoom(1.0 * d, area_w() / 2.0, area_h() / 2.0); A.offx = A.offy = 0; gtk_widget_queue_draw(A.area); });
    add("zoom_fit", "Fit to window", "View", "zoom-fit-best", K(0), NOMOD, [] { fit(true); });
    add("zoom_fit_width", "Fit width", "View", "zoom-fit-best", K(w), NOMOD, [] {
        if (!img()) return;
        double w, h; rotated_size(w, h);
        A.zoom = area_w() / w; A.offx = 0; A.offy = std::max(0.0, (h * A.zoom - area_h()) / 2);   // show the top
        A.auto_fit = false; gtk_widget_queue_draw(A.area); update_status(); });
    add("fit_on_open", "Fit large images on open", "View", "zoom-fit-best", 0, NOMOD, [] { A.fit_on_open = !A.fit_on_open; }, [] { return A.fit_on_open; });
    add("fullscreen", "Full screen", "View", "view-fullscreen", K(F11), NOMOD, toggle_fullscreen, [] { return A.fullscreen; });
    add("background", "Cycle background", "View", "preferences-desktop-wallpaper", K(b), NOMOD, [] { A.bg = (A.bg + 1) % BG_COUNT; gtk_widget_queue_draw(A.area); });
    add("smooth", "Smooth when zoomed in", "View", "image-x-generic", K(p), NOMOD, [] { A.smooth = !A.smooth; gtk_widget_queue_draw(A.area); }, [] { return A.smooth; });
    add("tiled", "Tiled view (seamless check)", "View", "view-grid", K(t), NOMOD, [] { A.tiled = !A.tiled; gtk_widget_queue_draw(A.area); }, [] { return A.tiled; });
    add("wheel_zoom", "Mouse wheel zooms (off = navigates)", "View", "input-mouse", K(m), NOMOD, [] { A.wheel_zoom = !A.wheel_zoom; }, [] { return A.wheel_zoom; });
    add("toolbar", "Show toolbar", "View", "view-more", K(t), CTRL, [] { A.show_toolbar = !A.show_toolbar; gtk_widget_set_visible(A.toolbar, A.show_toolbar && !A.fullscreen); }, [] { return A.show_toolbar; });
    add("statusbar", "Show status bar", "View", "view-more-horizontal", K(b), CTRL, [] { A.show_status = !A.show_status; gtk_widget_set_visible(A.statusbar, A.show_status && !A.fullscreen); }, [] { return A.show_status; });
    add("on_top", "Always on top", "View", "go-top", 0, NOMOD, [] { A.on_top = !A.on_top; gtk_window_set_keep_above(GTK_WINDOW(A.win), A.on_top); }, [] { return A.on_top; });
    // Image
    add("rotate_left", "Rotate left", "Image", "object-rotate-left", K(l), NOMOD, [] { A.rot = (A.rot + 3) % 4; transform_changed(); });
    add("rotate_right", "Rotate right", "Image", "object-rotate-right", K(r), NOMOD, [] { A.rot = (A.rot + 1) % 4; transform_changed(); });
    add("flip_h", "Flip horizontal", "Image", "object-flip-horizontal", K(h), NOMOD, [] { A.flip_h = !A.flip_h; transform_changed(); });
    add("flip_v", "Flip vertical", "Image", "object-flip-vertical", K(v), NOMOD, [] { A.flip_v = !A.flip_v; transform_changed(); });
    add("reset_transform", "Reset rotation / flip", "Image", "edit-undo", K(r), CTRL, [] { A.rot = 0; A.flip_h = A.flip_v = false; transform_changed(); });
    // 3D
    add("f3d", "Open in f3d (3D viewer)", "3D", "applications-graphics", K(d), NOMOD, cmd_f3d);
    // Favorites
    add("customize_favorites", "Customize favorites...", "Favorites", "starred", 0, NOMOD, customize_favorites);
    add("show_menu", "All commands menu", "Favorites", "open-menu", K(Menu), NOMOD, [] { show_menu(nullptr); });
    // Help
    add("help", "Commands and shortcuts", "Help", "help-browser", K(F1), NOMOD, shortcuts_help);
    add("about", "About " APP_NAME, "Help", "help-about", 0, NOMOD, [] {
        gtk_show_about_dialog(GTK_WINDOW(A.win), "program-name", APP_NAME, "version", APP_VERSION,
            "comments", "Lightweight viewer for huge PSD/PSB and common image formats.", "logo-icon-name", "image-x-generic", nullptr); });

    aliases = {{K(Page_Down), NOMOD, "next"}, {K(space), NOMOD, "next"}, {K(Page_Up), NOMOD, "prev"}, {K(BackSpace), NOMOD, "prev"},
               {K(equal), NOMOD, "zoom_in"}, {K(KP_Add), NOMOD, "zoom_in"}, {K(KP_Subtract), NOMOD, "zoom_out"},
               {K(KP_Multiply), NOMOD, "zoom_100"}, {K(KP_Divide), NOMOD, "zoom_fit"}, {K(f), NOMOD, "fullscreen"},
               {K(Return), NOMOD, "fullscreen"}, {K(KP_1), NOMOD, "zoom_100"}, {K(KP_0), NOMOD, "zoom_fit"}};
}

static void run_cmd(int i) {
    if (i < 0 || i >= (int)cmds.size()) return;
    cmds[i].run();
    if (cmds[i].checked) save_settings();
}

// ---------------------------------------------------------------- favorites
static bool is_fav(const char* id) { return std::find(A.favs.begin(), A.favs.end(), id) != A.favs.end(); }

static void toggle_fav(const char* id) {
    auto it = std::find(A.favs.begin(), A.favs.end(), id);
    if (it != A.favs.end()) A.favs.erase(it); else A.favs.push_back(id);
    rebuild_toolbar();
    save_settings();
}

static std::string menu_label(const Command& c) { return std::string(is_fav(c.id) ? "★ " : "") + c.label; }

static gboolean on_menu_item_release(GtkWidget* w, GdkEventButton* ev, gpointer data) {
    if (ev->button != 3) return FALSE;                  // left click: normal activate
    const Command& c = cmds[GPOINTER_TO_INT(data)];
    toggle_fav(c.id);
    gtk_menu_item_set_label(GTK_MENU_ITEM(w), menu_label(c).c_str());
    return TRUE;                                        // keep the menu open
}

static void on_menu_item_activate(GtkMenuItem*, gpointer data) {
    int i = GPOINTER_TO_INT(data);
    // run after the menu has closed (dialogs, grabs)
    g_idle_add([](gpointer p) -> gboolean { run_cmd(GPOINTER_TO_INT(p)); return G_SOURCE_REMOVE; }, GINT_TO_POINTER(i));
}

static GtkWidget* build_menu() {
    GtkWidget* menu = gtk_menu_new();
    for (const char* g : kGroups) {
        GtkWidget* gi = gtk_menu_item_new_with_label(g);
        GtkWidget* sub = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(gi), sub);
        for (size_t i = 0; i < cmds.size(); i++) {
            const Command& c = cmds[i];
            if (strcmp(c.group, g)) continue;
            GtkWidget* it;
            if (c.checked) { it = gtk_check_menu_item_new_with_label(menu_label(c).c_str()); gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), c.checked()); }
            else it = gtk_menu_item_new_with_label(menu_label(c).c_str());
            if (c.key) gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(it))), c.key, c.mods);
            gtk_widget_set_tooltip_text(it, "Right-click: add to / remove from Favorites toolbar");
            g_signal_connect(it, "activate", G_CALLBACK(on_menu_item_activate), GINT_TO_POINTER((int)i));
            g_signal_connect(it, "button-release-event", G_CALLBACK(on_menu_item_release), GINT_TO_POINTER((int)i));
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), it);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gi);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget* hint = gtk_menu_item_new_with_label("★ Right-click a command to pin it to the toolbar");
    gtk_widget_set_sensitive(hint, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), hint);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkWidget* m, gpointer) {
        g_idle_add([](gpointer p) -> gboolean { gtk_widget_destroy(GTK_WIDGET(p)); return G_SOURCE_REMOVE; }, m); }), nullptr);
    return menu;
}

static void show_menu(GdkEvent* ev) {
    GtkWidget* menu = build_menu();
    gtk_menu_attach_to_widget(GTK_MENU(menu), A.area, nullptr);
    if (ev) gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
    else gtk_menu_popup_at_widget(GTK_MENU(menu), A.area, GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
}

static void fav_move(const std::string& id, int d) {
    auto it = std::find(A.favs.begin(), A.favs.end(), id);
    if (it == A.favs.end()) return;
    long i = it - A.favs.begin(), j = i + d;
    if (j < 0 || j >= (long)A.favs.size()) return;
    std::swap(A.favs[i], A.favs[j]);
    rebuild_toolbar(); save_settings();
}

static gboolean on_toolbutton_press(GtkWidget*, GdkEventButton* ev, gpointer data) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    std::string id = cmds[GPOINTER_TO_INT(data)].id;
    GtkWidget* m = gtk_menu_new();
    auto item = [m](const char* label, std::function<void()> f) {
        GtkWidget* it = gtk_menu_item_new_with_label(label);
        auto* fp = new std::function<void()>(std::move(f));
        g_signal_connect_data(it, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer p) {
            auto* fn = new std::function<void()>(*(std::function<void()>*)p);
            g_idle_add([](gpointer q) -> gboolean { auto* f2 = (std::function<void()>*)q; (*f2)(); delete f2; return G_SOURCE_REMOVE; }, fn);
        }), fp, [](gpointer p, GClosure*) { delete (std::function<void()>*)p; }, (GConnectFlags)0);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
    };
    item("Remove from favorites", [id] { toggle_fav(id.c_str()); });
    item("Move left", [id] { fav_move(id, -1); });
    item("Move right", [id] { fav_move(id, 1); });
    item("Customize favorites...", customize_favorites);
    gtk_widget_show_all(m);
    g_signal_connect(m, "deactivate", G_CALLBACK(+[](GtkWidget* w, gpointer) {
        g_idle_add([](gpointer p) -> gboolean { gtk_widget_destroy(GTK_WIDGET(p)); return G_SOURCE_REMOVE; }, w); }), nullptr);
    gtk_menu_attach_to_widget(GTK_MENU(m), A.toolbar, nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent*)ev);
    return TRUE;
}

static void rebuild_toolbar() {
    GList* ch = gtk_container_get_children(GTK_CONTAINER(A.toolbar));
    for (GList* l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
    GtkIconTheme* theme = gtk_icon_theme_get_default();
    for (auto& id : A.favs) {
        int i = cmd_index(id);
        if (i < 0) continue;
        const Command& c = cmds[i];
        GtkToolItem* b;
        if (c.checked) { b = gtk_toggle_tool_button_new(); gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(b), c.checked()); }
        else b = gtk_tool_button_new(nullptr, nullptr);
        std::string icon = std::string(c.icon) + "-symbolic";      // symbolic icons follow the dark theme
        if (!gtk_icon_theme_has_icon(theme, icon.c_str())) icon = c.icon;
        bool has_icon = gtk_icon_theme_has_icon(theme, icon.c_str());
        if (has_icon) gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(b), icon.c_str());
        gtk_tool_button_set_label(GTK_TOOL_BUTTON(b), c.label);
        gtk_tool_item_set_is_important(b, !has_icon);    // no icon -> show its text
        std::string tip = c.label;
        if (c.key) { gchar* a = gtk_accelerator_get_label(c.key, c.mods); tip += "  (" + std::string(a) + ")"; g_free(a); }
        tip += "\nRight-click for options";
        gtk_widget_set_tooltip_text(GTK_WIDGET(b), tip.c_str());
        g_signal_connect(b, c.checked ? "toggled" : "clicked", G_CALLBACK(+[](GtkToolButton* tb, gpointer p) {
            if (g_object_get_data(G_OBJECT(tb), "syncing")) return;
            run_cmd(GPOINTER_TO_INT(p));
            rebuild_toolbar();                           // refresh toggle states
        }), GINT_TO_POINTER(i));
        gtk_widget_set_focus_on_click(GTK_WIDGET(b), FALSE);
        GtkWidget* inner = gtk_bin_get_child(GTK_BIN(b));
        if (inner) {
            gtk_widget_set_can_focus(inner, FALSE);
            g_signal_connect(inner, "button-press-event", G_CALLBACK(on_toolbutton_press), GINT_TO_POINTER(i));
        }
        gtk_toolbar_insert(GTK_TOOLBAR(A.toolbar), b, -1);
    }
    GtkToolItem* sep = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(sep), FALSE);
    gtk_tool_item_set_expand(sep, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(A.toolbar), sep, -1);
    GtkToolItem* mb = gtk_tool_button_new(nullptr, "All commands");
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(mb), gtk_icon_theme_has_icon(theme, "open-menu-symbolic") ? "open-menu-symbolic" : "view-more");
    gtk_widget_set_tooltip_text(GTK_WIDGET(mb), "All commands (right-click a command to pin it here)");
    g_signal_connect(mb, "clicked", G_CALLBACK(+[](GtkToolButton*, gpointer) { show_menu(gtk_get_current_event()); }), nullptr);
    if (GtkWidget* inner = gtk_bin_get_child(GTK_BIN(mb))) gtk_widget_set_can_focus(inner, FALSE);
    gtk_toolbar_insert(GTK_TOOLBAR(A.toolbar), mb, -1);
    gtk_widget_show_all(A.toolbar);
    gtk_widget_set_visible(A.toolbar, A.show_toolbar && !A.fullscreen);
}

static void customize_favorites() {
    enum { COL_ON, COL_LABEL, COL_GROUP, COL_KEYS, COL_ID, NCOL };
    GtkListStore* st = gtk_list_store_new(NCOL, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    auto add_row = [st](const Command& c, bool on) {
        std::string keys;
        if (c.key) { gchar* a = gtk_accelerator_get_label(c.key, c.mods); keys = a; g_free(a); }
        GtkTreeIter it; gtk_list_store_append(st, &it);
        gtk_list_store_set(st, &it, COL_ON, on, COL_LABEL, c.label, COL_GROUP, c.group, COL_KEYS, keys.c_str(), COL_ID, c.id, -1);
    };
    for (auto& id : A.favs) if (int i = cmd_index(id); i >= 0) add_row(cmds[i], true);
    for (auto& c : cmds) if (!is_fav(c.id)) add_row(c, false);

    GtkWidget* d = gtk_dialog_new_with_buttons("Customize favorites", GTK_WINDOW(A.win), GTK_DIALOG_MODAL,
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget* hint = gtk_label_new("Tick the commands to show on the toolbar. Drag rows to change their order.");
    gtk_widget_set_margin_top(hint, 8); gtk_widget_set_margin_bottom(hint, 8);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    GtkWidget* tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st));
    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(tv), TRUE);
    GtkCellRenderer* tog = gtk_cell_renderer_toggle_new();
    g_signal_connect(tog, "toggled", G_CALLBACK(+[](GtkCellRendererToggle*, gchar* path, gpointer m) {
        GtkTreeIter it; gboolean v;
        if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(m), &it, path)) return;
        gtk_tree_model_get(GTK_TREE_MODEL(m), &it, 0, &v, -1);
        gtk_list_store_set(GTK_LIST_STORE(m), &it, 0, !v, -1);
    }), st);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), gtk_tree_view_column_new_with_attributes("★", tog, "active", COL_ON, nullptr));
    const char* titles[] = {"Command", "Group", "Shortcut"};
    for (int k = 0; k < 3; k++)
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), gtk_tree_view_column_new_with_attributes(titles[k], gtk_cell_renderer_text_new(), "text", COL_LABEL + k, nullptr));
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(sw, 520, 480);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        A.favs.clear();
        GtkTreeIter it;
        for (gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(st), &it); ok; ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(st), &it)) {
            gboolean on; gchar* id;
            gtk_tree_model_get(GTK_TREE_MODEL(st), &it, COL_ON, &on, COL_ID, &id, -1);
            if (on) A.favs.push_back(id);
            g_free(id);
        }
        rebuild_toolbar(); save_settings();
    }
    gtk_widget_destroy(d);
    g_object_unref(st);
}

// ---------------------------------------------------------------- input
static gboolean on_key(GtkWidget*, GdkEventKey* ev, gpointer) {
    guint key = gdk_keyval_to_lower(ev->keyval);
    GdkModifierType m = (GdkModifierType)(ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK));
    if (key == GDK_KEY_Escape) {
        if (A.fullscreen) toggle_fullscreen();
        else if (A.slideshow_id) run_cmd(cmd_index("slideshow"));
        else gtk_widget_destroy(A.win);
        return TRUE;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < cmds.size(); i++)
            if (cmds[i].key && gdk_keyval_to_lower(cmds[i].key) == key && cmds[i].mods == m) { run_cmd((int)i); return TRUE; }
        for (auto& a : aliases)
            if (a.key == key && a.mods == m) { run_cmd(cmd_index(a.id)); return TRUE; }
        m = (GdkModifierType)(m & ~GDK_SHIFT_MASK);      // '+' etc. need Shift on many layouts
    }
    if (img() && (ev->state & GDK_SHIFT_MASK)) {         // Shift+arrows pan
        double step = 64;
        switch (key) {
        case GDK_KEY_Left: A.offx += step; break; case GDK_KEY_Right: A.offx -= step; break;
        case GDK_KEY_Up: A.offy += step; break; case GDK_KEY_Down: A.offy -= step; break;
        default: return FALSE;
        }
        A.auto_fit = false; gtk_widget_queue_draw(A.area); return TRUE;
    }
    if (img() && (key == GDK_KEY_Up || key == GDK_KEY_Down)) {
        A.offy += key == GDK_KEY_Up ? 64 : -64; A.auto_fit = false; gtk_widget_queue_draw(A.area); return TRUE;
    }
    return FALSE;
}

static void update_pixel(double sx, double sy) {
    Image* im = img();
    A.pixel_text.clear();
    if (im) {
        cairo_matrix_t m; view_matrix(&m);
        if (cairo_matrix_invert(&m) == CAIRO_STATUS_SUCCESS) {
            cairo_matrix_transform_point(&m, &sx, &sy);
            int x = (int)floor(sx), y = (int)floor(sy);
            uint8_t p[4];
            if (im->pixel(x, y, p)) {
                int d = A.cur->info.scale_div;
                char b[96];
                snprintf(b, sizeof b, "%d, %d   #%02X%02X%02X%s", x * d, y * d, p[0], p[1], p[2], "");
                A.pixel_text = b;
                if (im->has_alpha) A.pixel_text += "  a" + std::to_string(p[3]);
            }
        }
    }
    update_status();
}

static gboolean on_press(GtkWidget* w, GdkEventButton* ev, gpointer) {
    gtk_widget_grab_focus(w);
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) { toggle_fullscreen(); return TRUE; }
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    switch (ev->button) {
    case 1: A.dragging = true; A.drag_x = ev->x; A.drag_y = ev->y; return TRUE;
    case 2: fit(true); return TRUE;
    case 3: show_menu((GdkEvent*)ev); return TRUE;
    case 8: go(-1); return TRUE;
    case 9: go(1); return TRUE;
    }
    return FALSE;
}

static gboolean on_release(GtkWidget*, GdkEventButton* ev, gpointer) {
    if (ev->button == 1) A.dragging = false;
    return TRUE;
}

static gboolean on_motion(GtkWidget*, GdkEventMotion* ev, gpointer) {
    if (A.dragging) {
        A.offx += ev->x - A.drag_x; A.offy += ev->y - A.drag_y;
        A.drag_x = ev->x; A.drag_y = ev->y;
        A.auto_fit = false;
        gtk_widget_queue_draw(A.area);
    }
    update_pixel(ev->x, ev->y);
    return TRUE;
}

static gboolean on_scroll(GtkWidget*, GdkEventScroll* ev, gpointer) {
    int dir = 0;
    if (ev->direction == GDK_SCROLL_UP) dir = 1;
    else if (ev->direction == GDK_SCROLL_DOWN) dir = -1;
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        static double accum = 0;
        accum += ev->delta_y;
        if (accum <= -1) { dir = 1; accum = 0; } else if (accum >= 1) { dir = -1; accum = 0; }
    }
    if (!dir) return TRUE;
    bool zoom = A.wheel_zoom != ((ev->state & GDK_CONTROL_MASK) != 0);   // Ctrl inverts
    if (zoom) zoom_step(dir, ev->x, ev->y); else go(-dir);
    return TRUE;
}

static void on_size(GtkWidget*, GdkRectangle*, gpointer) { if (A.auto_fit && img()) fit(A.zoom > 1.0001); }

static void on_drop(GtkWidget*, GdkDragContext* ctx, int, int, GtkSelectionData* sd, guint, guint t, gpointer) {
    gchar** uris = gtk_selection_data_get_uris(sd);
    if (uris && uris[0]) {
        gchar* f = g_filename_from_uri(uris[0], nullptr, nullptr);
        if (f) { open_file(f); g_free(f); }
    }
    g_strfreev(uris);
    gtk_drag_finish(ctx, TRUE, FALSE, t);
}

// ---------------------------------------------------------------- main
// Dark UI everywhere: ask the theme for its dark variant (menus, dialogs, tooltips), and
// add CSS so toolbar, status bar and menus are dark even with themes lacking a dark variant.
static void apply_dark_theme() {
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
    static const char* css =
        "window, dialog, .background { background-color: #1e1f22; color: #d8d8dc; }"
        "button { background-image: none; background-color: #33353a; color: #e0e0e4; border: 1px solid #45474e; box-shadow: none; text-shadow: none; }"
        "button:hover { background-color: #3d4046; }"
        "button:active, button:checked { background-color: #44506a; }"
        "button.suggested-action { background-color: #3d5a8a; border-color: #4a6aa0; }"
        "toolbar, .toolbar { background: #26272b; border-bottom: 1px solid #111214; padding: 2px 4px; }"
        "toolbar button, toolbar toolbutton > button { color: #d8d8dc; background: transparent; border-color: transparent; box-shadow: none; }"
        "toolbar button:hover { background: #3a3c42; }"
        "toolbar button:checked { background: #44506a; }"
        "#statusbar { background: #26272b; border-top: 1px solid #111214; padding: 3px 8px; }"
        "#statusbar label { color: #b8b9bf; }"
        "menu, .menu, .context-menu { background-color: #2b2d31; color: #e0e0e4; border: 1px solid #111214; }"
        "menuitem { color: #e0e0e4; }"
        "menuitem:hover { background-color: #3d5a8a; color: #ffffff; }"
        "menuitem:disabled, menuitem:disabled label { color: #7d7f87; }"
        "menuitem accelerator { color: #8f9199; }"
        "tooltip, tooltip.background { background-color: #2b2d31; color: #e0e0e4; }"
        "treeview.view { background-color: #232428; color: #d8d8dc; }"
        "treeview.view:selected { background-color: #3d5a8a; color: #ffffff; }"
        "entry { background-color: #2b2d31; color: #e0e0e4; border-color: #44464d; }"
        "separator { background-color: #3a3c42; }";
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, nullptr);
    // APPLICATION priority: above the theme, below the user's own gtk.css
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

// hb_viewer --convert <in> <out.png|jpg|bmp|tiff> [max_megapixels]
static int cli_convert(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: hb_viewer --convert <input> <output.png|jpg|bmp|tiff> [max_megapixels]\n"); return 2; }
    LoadCtl ctl;
    uint64_t budget = argc > 4 ? (uint64_t)atoll(argv[4]) * 1000000 : 2000000000ull;
    auto t0 = std::chrono::steady_clock::now();
    auto r = load_image(argv[2], budget, ctl);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!r->img) { fprintf(stderr, "%s: %s\n", argv[2], r->error.c_str()); return 1; }
    fprintf(stderr, "%s: %s %s %dx%d (1/%d) %.3fs\n", argv[2], r->info.format.c_str(), r->info.details.c_str(),
            r->info.orig_w, r->info.orig_h, r->info.scale_div, secs);
    GdkPixbuf* pb = pixbuf_from_image(r->img.get());
    if (!pb) { fprintf(stderr, "image too large to save\n"); return 1; }
    std::string e = lower_ext(argv[3]);
    const char* type = e == "jpg" || e == "jpeg" ? "jpeg" : e == "bmp" ? "bmp" : e == "tif" || e == "tiff" ? "tiff" : "png";
    GError* err = nullptr;
    if (!gdk_pixbuf_save(pb, argv[3], type, &err, nullptr)) { fprintf(stderr, "%s\n", err->message); return 1; }
    g_object_unref(pb);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--convert")) return cli_convert(argc, argv);
    gtk_init(&argc, &argv);
    apply_dark_theme();
    g_set_application_name(APP_NAME);
    register_commands();
    load_settings();

    A.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(A.win), APP_NAME);
    gtk_window_set_icon_name(GTK_WINDOW(A.win), "hb_viewer");
    {
        GError* e = nullptr;
        int w = g_key_file_get_integer(A.ini, "window", "width", &e); if (e) { g_clear_error(&e); w = 1200; }
        int h = g_key_file_get_integer(A.ini, "window", "height", &e); if (e) { g_clear_error(&e); h = 800; }
        gtk_window_set_default_size(GTK_WINDOW(A.win), std::max(w, 320), std::max(h, 240));
        if (g_key_file_get_boolean(A.ini, "window", "maximized", nullptr)) gtk_window_maximize(GTK_WINDOW(A.win));
    }
    if (const char* ad = g_getenv("APPDIR")) {
        std::string icon = std::string(ad) + "/hb_viewer.png";
        gtk_window_set_icon_from_file(GTK_WINDOW(A.win), icon.c_str(), nullptr);
    }

    GtkWidget* vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(A.win), vb);
    A.toolbar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(A.toolbar), GTK_TOOLBAR_BOTH_HORIZ);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(A.toolbar), GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_box_pack_start(GTK_BOX(vb), A.toolbar, FALSE, FALSE, 0);

    A.area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(A.area, TRUE);
    gtk_widget_add_events(A.area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                  GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    gtk_box_pack_start(GTK_BOX(vb), A.area, TRUE, TRUE, 0);
    g_signal_connect(A.area, "draw", G_CALLBACK(on_draw), nullptr);
    g_signal_connect(A.area, "button-press-event", G_CALLBACK(on_press), nullptr);
    g_signal_connect(A.area, "button-release-event", G_CALLBACK(on_release), nullptr);
    g_signal_connect(A.area, "motion-notify-event", G_CALLBACK(on_motion), nullptr);
    g_signal_connect(A.area, "scroll-event", G_CALLBACK(on_scroll), nullptr);
    g_signal_connect(A.area, "size-allocate", G_CALLBACK(on_size), nullptr);
    gtk_drag_dest_set(A.area, GTK_DEST_DEFAULT_ALL, nullptr, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(A.area);
    g_signal_connect(A.area, "drag-data-received", G_CALLBACK(on_drop), nullptr);

    A.statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_name(A.statusbar, "statusbar");
    A.status_l = gtk_label_new("");
    A.status_r = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(A.status_l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(A.status_l, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(A.statusbar), A.status_l, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(A.statusbar), A.status_r, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), A.statusbar, FALSE, FALSE, 0);

    g_signal_connect(A.win, "key-press-event", G_CALLBACK(on_key), nullptr);
    g_signal_connect(A.win, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean { save_settings(); return FALSE; }), nullptr);
    g_signal_connect(A.win, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { save_settings(); gtk_main_quit(); }), nullptr);

    gtk_widget_show_all(A.win);
    gtk_widget_set_visible(A.statusbar, A.show_status);
    rebuild_toolbar();
    gtk_widget_grab_focus(A.area);
    if (argc > 1) open_file(argv[1]);
    update_status();
    gtk_main();
    for (auto& j : A.jobs) j.second.ctl->cancel = true;
    _exit(0);                                            // don't wait for detached decoder threads
}
