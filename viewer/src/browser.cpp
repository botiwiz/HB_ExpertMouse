#include "browser.h"
#include "decode.h"
#include "thumbs.h"
#include <glib/gstdio.h>
#include <algorithm>
#include <cstring>
#include <map>

namespace {

enum { C_PIX, C_NAME, C_PATH, C_DIR, C_SIZE, C_MTIME, C_TYPE, C_NCOL };
enum { T_NAME, T_PATH, T_ICON, T_LOADED, T_NCOL };

struct Thumb { GdkPixbuf* pb; gint64 mtime; };

struct State {
    BrowserHost host;
    GtkWidget *paned, *tree, *iv, *entry, *sw;
    GtkTreeStore* ts;
    GtkListStore* ls;
    std::string folder;
    int gen = 0, size = 128, sort = SORT_NAME;
    bool reverse = false, hidden = false, syncing = false;
    std::map<std::string, GtkTreeIter> rows;
    std::map<std::string, Thumb> thumbs;               // 256 px originals, current folder
    std::map<std::string, GdkPixbuf*> icons;           // content type -> icon at current size
    GFileMonitor* monitor = nullptr;
    guint refresh_id = 0, prio_id = 0;
} S;

const int kSizes[] = {48, 64, 96, 128, 160, 192, 256, 320, 384};

// size x size transparent canvas with 'src' scaled to fit (max 'fit' px) and centred
GdkPixbuf* cell(GdkPixbuf* src, int size, int fit) {
    GdkPixbuf* c = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, size, size);
    gdk_pixbuf_fill(c, 0);
    if (!src) return c;
    int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src);
    double s = std::min((double)fit / w, (double)fit / h);
    int dw = std::max(1, (int)(w * s + 0.5)), dh = std::max(1, (int)(h * s + 0.5));
    int x = (size - dw) / 2, y = (size - dh) / 2;
    gdk_pixbuf_composite(src, c, x, y, dw, dh, x, y, (double)dw / w, (double)dh / h,
                         s < 1 ? GDK_INTERP_BILINEAR : GDK_INTERP_BILINEAR, 255);
    return c;
}

GdkPixbuf* type_icon(const std::string& name, bool dir) {
    std::string key;
    GIcon* gi;
    if (dir) { key = "inode/directory"; gi = g_themed_icon_new("folder"); }
    else {
        gboolean unc;
        gchar* ct = g_content_type_guess(name.c_str(), nullptr, 0, &unc);
        key = ct;
        gi = g_content_type_get_icon(ct);
        g_free(ct);
    }
    auto it = S.icons.find(key);
    if (it != S.icons.end()) { g_object_unref(gi); return it->second; }
    int px = std::max(16, S.size * 5 / 8);
    GdkPixbuf* ic = nullptr;
    if (GtkIconInfo* info = gtk_icon_theme_lookup_by_gicon(gtk_icon_theme_get_default(), gi, px, GTK_ICON_LOOKUP_FORCE_SIZE)) {
        ic = gtk_icon_info_load_icon(info, nullptr);
        g_object_unref(info);
    }
    if (!ic) ic = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), dir ? "folder" : "text-x-generic", px, GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
    g_object_unref(gi);
    GdkPixbuf* c = cell(ic, S.size, px);
    if (ic) g_object_unref(ic);
    S.icons[key] = c;
    return c;
}

void set_row_pixbuf(GtkTreeIter* it) {
    gchar *path, *name; gboolean dir;
    gtk_tree_model_get(GTK_TREE_MODEL(S.ls), it, C_PATH, &path, C_NAME, &name, C_DIR, &dir, -1);
    auto t = S.thumbs.find(path);
    if (t != S.thumbs.end() && t->second.pb) {
        GdkPixbuf* c = cell(t->second.pb, S.size, S.size);
        gtk_list_store_set(S.ls, it, C_PIX, c, -1);
        g_object_unref(c);
    } else gtk_list_store_set(S.ls, it, C_PIX, type_icon(name, dir), -1);
    g_free(path); g_free(name);
}

void on_thumb(int gen, const std::string& path, GdkPixbuf* pb) {
    if (gen != S.gen || !pb) return;
    auto r = S.rows.find(path);
    if (r == S.rows.end()) return;
    gint64 mt; gtk_tree_model_get(GTK_TREE_MODEL(S.ls), &r->second, C_MTIME, &mt, -1);
    auto old = S.thumbs.find(path);
    if (old != S.thumbs.end() && old->second.pb) g_object_unref(old->second.pb);
    g_object_ref(pb);
    S.thumbs[path] = {pb, mt};
    set_row_pixbuf(&r->second);
}

gboolean prioritize_visible(gpointer) {
    S.prio_id = 0;
    GtkTreePath *a = nullptr, *b = nullptr;
    if (!gtk_icon_view_get_visible_range(GTK_ICON_VIEW(S.iv), &a, &b)) return G_SOURCE_REMOVE;
    int i0 = gtk_tree_path_get_indices(a)[0], i1 = gtk_tree_path_get_indices(b)[0];
    gtk_tree_path_free(a); gtk_tree_path_free(b);
    std::vector<std::string> v;
    GtkTreeIter it;
    if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(S.ls), &it, nullptr, i0))
        for (int i = i0; i <= i1 + 8; i++) {
            gchar* p; gtk_tree_model_get(GTK_TREE_MODEL(S.ls), &it, C_PATH, &p, -1);
            v.push_back(p); g_free(p);
            if (!gtk_tree_model_iter_next(GTK_TREE_MODEL(S.ls), &it)) break;
        }
    thumbs_prioritize(v);
    return G_SOURCE_REMOVE;
}

void schedule_prioritize() { if (!S.prio_id) S.prio_id = g_timeout_add(60, prioritize_visible, nullptr); }

struct Entry { std::string name, path, key; bool dir; gint64 size, mtime; std::string ext; };

void list_folder(const std::vector<std::string>& select) {
    S.gen++;
    S.rows.clear();
    gtk_list_store_clear(S.ls);
    std::vector<Entry> v;
    if (GDir* d = g_dir_open(S.folder.c_str(), 0, nullptr)) {
        while (const char* n = g_dir_read_name(d)) {
            if (!S.hidden && n[0] == '.') continue;
            Entry e;
            e.name = n;
            e.path = S.folder == "/" ? "/" + e.name : S.folder + "/" + e.name;
            GStatBuf st;
            if (g_stat(e.path.c_str(), &st) != 0) { e.dir = false; e.size = 0; e.mtime = 0; }
            else { e.dir = S_ISDIR(st.st_mode); e.size = st.st_size; e.mtime = st.st_mtime; }
            gchar* k = g_utf8_collate_key_for_filename(n, -1); e.key = k; g_free(k);
            e.ext = lower_ext(e.name);
            v.push_back(std::move(e));
        }
        g_dir_close(d);
    }
    std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        int c = 0;
        switch (S.sort) {
        case SORT_DATE: c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime; break;
        case SORT_SIZE: c = a.size < b.size ? -1 : a.size > b.size; break;
        case SORT_TYPE: c = a.ext.compare(b.ext); break;
        }
        if (!c) c = a.key.compare(b.key);
        return S.reverse ? c > 0 : c < 0;
    });
    // drop thumbnails of files that vanished or changed
    for (auto it = S.thumbs.begin(); it != S.thumbs.end();) {
        auto f = std::find_if(v.begin(), v.end(), [&](const Entry& e) { return e.path == it->first; });
        if (f == v.end() || f->mtime != it->second.mtime) { if (it->second.pb) g_object_unref(it->second.pb); it = S.thumbs.erase(it); }
        else ++it;
    }
    std::vector<std::pair<std::string, bool>> req;
    for (auto& e : v) {
        GtkTreeIter it;
        gtk_list_store_insert_with_values(S.ls, &it, -1, C_NAME, e.name.c_str(), C_PATH, e.path.c_str(), C_DIR, e.dir,
                                          C_SIZE, e.size, C_MTIME, e.mtime, C_TYPE, e.ext.c_str(), -1);
        S.rows[e.path] = it;
        set_row_pixbuf(&it);
        if (!e.dir && !S.thumbs.count(e.path))           // images are generated, other types only read from the cache
            req.emplace_back(e.path, is_image_ext(e.ext));
    }
    thumbs_request(S.gen, req);
    gtk_icon_view_unselect_all(GTK_ICON_VIEW(S.iv));
    bool first = true;
    for (auto& p : select) {
        auto r = S.rows.find(p);
        if (r == S.rows.end()) continue;
        GtkTreePath* tp = gtk_tree_model_get_path(GTK_TREE_MODEL(S.ls), &r->second);
        gtk_icon_view_select_path(GTK_ICON_VIEW(S.iv), tp);
        if (first) { gtk_icon_view_set_cursor(GTK_ICON_VIEW(S.iv), tp, nullptr, FALSE); gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(S.iv), tp, TRUE, 0.5, 0); first = false; }
        gtk_tree_path_free(tp);
    }
    if (first && !v.empty()) {
        GtkTreePath* tp = gtk_tree_path_new_first();
        gtk_icon_view_select_path(GTK_ICON_VIEW(S.iv), tp);
        gtk_icon_view_set_cursor(GTK_ICON_VIEW(S.iv), tp, nullptr, FALSE);
        gtk_tree_path_free(tp);
    }
    schedule_prioritize();
    S.host.changed();
}

// ------------------------------------------------ folder tree
void tree_populate(GtkTreeIter* parent) {
    gboolean loaded; gchar* path;
    gtk_tree_model_get(GTK_TREE_MODEL(S.ts), parent, T_LOADED, &loaded, T_PATH, &path, -1);
    if (loaded) { g_free(path); return; }
    GtkTreeIter ch;
    while (gtk_tree_model_iter_children(GTK_TREE_MODEL(S.ts), &ch, parent)) gtk_tree_store_remove(S.ts, &ch);
    std::vector<std::pair<std::string, std::string>> dirs;
    if (GDir* d = g_dir_open(path, 0, nullptr)) {
        while (const char* n = g_dir_read_name(d)) {
            if (!S.hidden && n[0] == '.') continue;
            std::string p = std::string(path) + (strcmp(path, "/") ? "/" : "") + n;
            if (!g_file_test(p.c_str(), G_FILE_TEST_IS_DIR)) continue;
            gchar* k = g_utf8_collate_key_for_filename(n, -1);
            dirs.emplace_back(k, n); g_free(k);
        }
        g_dir_close(d);
    }
    std::sort(dirs.begin(), dirs.end());
    for (auto& dn : dirs) {
        std::string p = std::string(path) + (strcmp(path, "/") ? "/" : "") + dn.second;
        GtkTreeIter it, dummy;
        gtk_tree_store_insert_with_values(S.ts, &it, parent, -1, T_NAME, dn.second.c_str(), T_PATH, p.c_str(), T_ICON, "folder", T_LOADED, FALSE, -1);
        gtk_tree_store_insert_with_values(S.ts, &dummy, &it, -1, T_NAME, "", T_PATH, "", T_LOADED, TRUE, -1);
    }
    gtk_tree_store_set(S.ts, parent, T_LOADED, TRUE, -1);
    g_free(path);
}

void tree_add_root(const char* label, const std::string& path, const char* icon) {
    GtkTreeIter it, dummy;
    gtk_tree_store_insert_with_values(S.ts, &it, nullptr, -1, T_NAME, label, T_PATH, path.c_str(), T_ICON, icon, T_LOADED, FALSE, -1);
    gtk_tree_store_insert_with_values(S.ts, &dummy, &it, -1, T_NAME, "", T_PATH, "", T_LOADED, TRUE, -1);
}

void tree_sync() {
    // pick the root whose path is the longest prefix of the folder, then walk down
    GtkTreeIter it, best; size_t best_len = 0; bool found = false;
    for (gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(S.ts), &it); ok; ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(S.ts), &it)) {
        gchar* p; gtk_tree_model_get(GTK_TREE_MODEL(S.ts), &it, T_PATH, &p, -1);
        std::string rp = p; g_free(p);
        bool pre = S.folder == rp || (S.folder.rfind(rp == "/" ? rp : rp + "/", 0) == 0);
        if (pre && rp.size() >= best_len) { best = it; best_len = rp.size(); found = true; }
    }
    if (!found) return;
    S.syncing = true;
    GtkTreeIter cur = best;
    gchar* rp; gtk_tree_model_get(GTK_TREE_MODEL(S.ts), &cur, T_PATH, &rp, -1);
    std::string rest = S.folder.substr(strlen(rp));
    g_free(rp);
    gchar** parts = g_strsplit(rest.c_str(), "/", -1);
    for (gchar** p = parts; *p; p++) {
        if (!**p) continue;
        tree_populate(&cur);
        GtkTreePath* tp = gtk_tree_model_get_path(GTK_TREE_MODEL(S.ts), &cur);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(S.tree), tp, FALSE);
        gtk_tree_path_free(tp);
        GtkTreeIter ch; bool hit = false;
        for (gboolean ok = gtk_tree_model_iter_children(GTK_TREE_MODEL(S.ts), &ch, &cur); ok; ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(S.ts), &ch)) {
            gchar* n; gtk_tree_model_get(GTK_TREE_MODEL(S.ts), &ch, T_NAME, &n, -1);
            hit = !strcmp(n, *p); g_free(n);
            if (hit) break;
        }
        if (!hit) break;                                 // e.g. hidden folder
        cur = ch;
    }
    g_strfreev(parts);
    GtkTreePath* tp = gtk_tree_model_get_path(GTK_TREE_MODEL(S.ts), &cur);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(S.tree)), tp);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(S.tree), tp, nullptr, FALSE, 0, 0);
    gtk_tree_path_free(tp);
    S.syncing = false;
}

void tree_rebuild() {
    gtk_tree_store_clear(S.ts);
    tree_add_root("Home", g_get_home_dir(), "user-home");
    std::string media = std::string("/media/") + g_get_user_name();
    if (g_file_test(media.c_str(), G_FILE_TEST_IS_DIR)) tree_add_root("Drives", media, "drive-removable-media");
    tree_add_root("File system", "/", "drive-harddisk");
}

void on_monitor(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev, gpointer) {
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) {}
    if (S.refresh_id) g_source_remove(S.refresh_id);
    S.refresh_id = g_timeout_add(400, [](gpointer) -> gboolean { S.refresh_id = 0; browser_refresh(); return G_SOURCE_REMOVE; }, nullptr);
}

} // namespace

// ------------------------------------------------ public
GtkWidget* browser_create(BrowserHost host) {
    S.host = std::move(host);
    thumbs_start(on_thumb);

    S.paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    // left: folder tree
    S.ts = gtk_tree_store_new(T_NCOL, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    S.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(S.ts));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(S.tree), FALSE);
    GtkTreeViewColumn* col = gtk_tree_view_column_new();
    GtkCellRenderer* ri = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer* rt = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, ri, FALSE);
    gtk_tree_view_column_pack_start(col, rt, TRUE);
    gtk_tree_view_column_add_attribute(col, ri, "icon-name", T_ICON);
    gtk_tree_view_column_add_attribute(col, rt, "text", T_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(S.tree), col);
    g_signal_connect(S.tree, "test-expand-row", G_CALLBACK(+[](GtkTreeView*, GtkTreeIter* it, GtkTreePath*, gpointer) -> gboolean {
        tree_populate(it); return FALSE; }), nullptr);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(S.tree)), "changed", G_CALLBACK(+[](GtkTreeSelection* sel, gpointer) {
        if (S.syncing) return;
        GtkTreeIter it;
        if (!gtk_tree_selection_get_selected(sel, nullptr, &it)) return;
        gchar* p; gtk_tree_model_get(GTK_TREE_MODEL(S.ts), &it, T_PATH, &p, -1);
        if (*p && S.folder != p) browser_set_folder(p);
        g_free(p);
    }), nullptr);
    GtkWidget* tsw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(tsw), S.tree);
    gtk_paned_pack1(GTK_PANED(S.paned), tsw, FALSE, TRUE);
    tree_rebuild();

    // right: path bar + icon grid
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(bar, "pathbar");
    GtkWidget* up = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(up, "Parent folder (Backspace)");
    gtk_widget_set_can_focus(up, FALSE);
    g_signal_connect(up, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { browser_go_up(); }), nullptr);
    S.entry = gtk_entry_new();
    g_signal_connect(S.entry, "activate", G_CALLBACK(+[](GtkEntry* e, gpointer) {
        gchar* p = g_canonicalize_filename(gtk_entry_get_text(e), S.folder.c_str());
        if (g_file_test(p, G_FILE_TEST_IS_DIR)) browser_set_folder(p);
        else if (g_file_test(p, G_FILE_TEST_EXISTS)) { gchar* d = g_path_get_dirname(p); browser_set_folder(d, p); g_free(d); }
        else gtk_entry_set_text(e, S.folder.c_str());
        g_free(p);
        browser_focus();
    }), nullptr);
    gtk_box_pack_start(GTK_BOX(bar), up, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), S.entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(right), bar, FALSE, FALSE, 0);

    S.ls = gtk_list_store_new(C_NCOL, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_INT64, G_TYPE_INT64, G_TYPE_STRING);
    S.iv = gtk_icon_view_new_with_model(GTK_TREE_MODEL(S.ls));
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(S.iv), GTK_SELECTION_MULTIPLE);
    GtkCellRenderer* cp = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer* ct = gtk_cell_renderer_text_new();
    g_object_set(ct, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, "xalign", 0.5, "alignment", PANGO_ALIGN_CENTER, nullptr);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(S.iv), cp, FALSE);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(S.iv), ct, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(S.iv), cp, "pixbuf", C_PIX);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(S.iv), ct, "text", C_NAME);
    g_object_set_data(G_OBJECT(S.iv), "text-renderer", ct);
    gtk_icon_view_set_item_padding(GTK_ICON_VIEW(S.iv), 4);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(S.iv), 6);
    gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(S.iv), 6);
    g_signal_connect(S.iv, "item-activated", G_CALLBACK(+[](GtkIconView*, GtkTreePath* tp, gpointer) {
        GtkTreeIter it;
        if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(S.ls), &it, tp)) return;
        gchar* p; gboolean dir;
        gtk_tree_model_get(GTK_TREE_MODEL(S.ls), &it, C_PATH, &p, C_DIR, &dir, -1);
        std::string path = p; g_free(p);
        if (dir) browser_set_folder(path); else S.host.activate(path);
    }), nullptr);
    g_signal_connect(S.iv, "selection-changed", G_CALLBACK(+[](GtkIconView*, gpointer) { S.host.changed(); }), nullptr);
    g_signal_connect(S.iv, "button-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventButton* ev, gpointer) -> gboolean {
        if (ev->type != GDK_BUTTON_PRESS) return FALSE;
        if (ev->button == 8) { browser_go_up(); return TRUE; }
        if (ev->button != 3) return FALSE;
        gtk_widget_grab_focus(w);
        GtkTreePath* tp = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(w), (int)ev->x, (int)ev->y);
        if (tp) {
            if (!gtk_icon_view_path_is_selected(GTK_ICON_VIEW(w), tp)) {
                gtk_icon_view_unselect_all(GTK_ICON_VIEW(w));
                gtk_icon_view_select_path(GTK_ICON_VIEW(w), tp);
            }
            gtk_icon_view_set_cursor(GTK_ICON_VIEW(w), tp, nullptr, FALSE);
            gtk_tree_path_free(tp);
        }
        S.host.context_menu((GdkEvent*)ev);
        return TRUE;
    }), nullptr);
    S.sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(S.sw), S.iv);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(S.sw)), "value-changed",
                     G_CALLBACK(+[](GtkAdjustment*, gpointer) { schedule_prioritize(); }), nullptr);
    g_signal_connect(S.iv, "size-allocate", G_CALLBACK(+[](GtkWidget*, GdkRectangle*, gpointer) { schedule_prioritize(); }), nullptr);
    gtk_box_pack_start(GTK_BOX(right), S.sw, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(S.paned), right, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(S.paned), 240);
    browser_set_thumb_size(S.size);
    return S.paned;
}

bool browser_set_folder(const std::string& dir, const std::string& select) {
    gchar* c = g_canonicalize_filename(dir.c_str(), nullptr);
    std::string d = c; g_free(c);
    if (!g_file_test(d.c_str(), G_FILE_TEST_IS_DIR)) return false;
    bool same = d == S.folder;
    std::string prev = S.folder;
    S.folder = d;
    gtk_entry_set_text(GTK_ENTRY(S.entry), d.c_str());
    if (!same) {
        for (auto& t : S.thumbs) if (t.second.pb) g_object_unref(t.second.pb);
        S.thumbs.clear();
        if (S.monitor) { g_file_monitor_cancel(S.monitor); g_object_unref(S.monitor); S.monitor = nullptr; }
        GFile* f = g_file_new_for_path(d.c_str());
        S.monitor = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
        g_object_unref(f);
        if (S.monitor) g_signal_connect(S.monitor, "changed", G_CALLBACK(on_monitor), nullptr);
    }
    std::vector<std::string> sel;
    if (!select.empty()) sel.push_back(select);
    else if (!same && !prev.empty() && prev.rfind(d == "/" ? d : d + "/", 0) == 0) {
        // going up: select the folder we came from
        size_t slash = prev.find('/', d.size() + (d == "/" ? 0 : 1));
        sel.push_back(prev.substr(0, slash));
    }
    list_folder(sel);
    tree_sync();
    return true;
}

const std::string& browser_folder() { return S.folder; }

std::vector<std::string> browser_selected() {
    std::vector<std::string> v;
    GList* l = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(S.iv));
    l = g_list_reverse(l);
    for (GList* x = l; x; x = x->next) {
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(S.ls), &it, (GtkTreePath*)x->data)) {
            gchar* p; gtk_tree_model_get(GTK_TREE_MODEL(S.ls), &it, C_PATH, &p, -1);
            v.push_back(p); g_free(p);
        }
    }
    g_list_free_full(l, (GDestroyNotify)gtk_tree_path_free);
    // keep the cursor item first
    GtkTreePath* cp = nullptr;
    if (gtk_icon_view_get_cursor(GTK_ICON_VIEW(S.iv), &cp, nullptr) && cp) {
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(S.ls), &it, cp)) {
            gchar* p; gtk_tree_model_get(GTK_TREE_MODEL(S.ls), &it, C_PATH, &p, -1);
            auto f = std::find(v.begin(), v.end(), std::string(p));
            if (f != v.end()) std::rotate(v.begin(), f, f + 1);
            g_free(p);
        }
        gtk_tree_path_free(cp);
    }
    return v;
}

void browser_refresh() {
    if (S.folder.empty()) return;
    if (!g_file_test(S.folder.c_str(), G_FILE_TEST_IS_DIR)) { browser_go_up(); return; }
    // keep scroll position and selection
    GtkAdjustment* va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(S.sw));
    double pos = gtk_adjustment_get_value(va);
    auto sel = browser_selected();
    list_folder(sel);
    gtk_adjustment_set_value(va, pos);
}

void browser_go_up() {
    if (S.folder.empty() || S.folder == "/") return;
    gchar* d = g_path_get_dirname(S.folder.c_str());
    browser_set_folder(d);
    g_free(d);
}

void browser_focus() { gtk_widget_grab_focus(S.iv); }

int browser_thumb_size() { return S.size; }

void browser_set_thumb_size(int px) {
    S.size = std::clamp(px, kSizes[0], kSizes[G_N_ELEMENTS(kSizes) - 1]);
    for (auto& i : S.icons) g_object_unref(i.second);
    S.icons.clear();
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(S.iv), S.size + 8);
    if (auto* ct = (GtkCellRenderer*)g_object_get_data(G_OBJECT(S.iv), "text-renderer"))
        g_object_set(ct, "width", S.size + 8, nullptr);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(S.ls), &it); ok; ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(S.ls), &it))
        set_row_pixbuf(&it);
    schedule_prioritize();
}

void browser_thumb_step(int dir) {
    int n = G_N_ELEMENTS(kSizes), i = 0;
    while (i < n - 1 && kSizes[i] < S.size) i++;
    i = std::clamp(i + dir, 0, n - 1);
    browser_set_thumb_size(kSizes[i]);
    GtkTreePath* cp = nullptr;
    if (gtk_icon_view_get_cursor(GTK_ICON_VIEW(S.iv), &cp, nullptr) && cp) {
        gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(S.iv), cp, TRUE, 0.5, 0);
        gtk_tree_path_free(cp);
    }
}

void browser_set_options(int sort, bool reverse, bool hidden) {
    bool hid_changed = hidden != S.hidden;
    S.sort = sort; S.reverse = reverse; S.hidden = hidden;
    if (hid_changed) { tree_rebuild(); if (!S.folder.empty()) tree_sync(); }
    browser_refresh();
}
int browser_sort() { return S.sort; }
bool browser_reverse() { return S.reverse; }
bool browser_hidden() { return S.hidden; }
int browser_pane_pos() { return gtk_paned_get_position(GTK_PANED(S.paned)); }
void browser_set_pane_pos(int px) { gtk_paned_set_position(GTK_PANED(S.paned), std::max(0, px)); }

static std::string hsize(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"}; int k = 0;
    while (b >= 1024 && k < 4) { b /= 1024; k++; }
    char s[32]; snprintf(s, sizeof s, k ? "%.1f %s" : "%.0f %s", b, u[k]); return s;
}

std::string browser_status() {
    int dirs = 0, files = 0, images = 0; gint64 total = 0;
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(S.ls), &it); ok; ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(S.ls), &it)) {
        gboolean d; gint64 sz; gchar* e;
        gtk_tree_model_get(GTK_TREE_MODEL(S.ls), &it, C_DIR, &d, C_SIZE, &sz, C_TYPE, &e, -1);
        if (d) dirs++; else { files++; total += sz; if (is_image_ext(e)) images++; }
        g_free(e);
    }
    std::string s = std::to_string(dirs) + " folders, " + std::to_string(files) + " files (" + std::to_string(images) +
                    " images, " + hsize((double)total) + ")";
    auto sel = browser_selected();
    if (sel.size() == 1) {
        auto r = S.rows.find(sel[0]);
        if (r != S.rows.end()) {
            gchar* n; gboolean d; gint64 sz, mt;
            gtk_tree_model_get(GTK_TREE_MODEL(S.ls), &r->second, C_NAME, &n, C_DIR, &d, C_SIZE, &sz, C_MTIME, &mt, -1);
            GDateTime* dt = g_date_time_new_from_unix_local(mt);
            gchar* ds = g_date_time_format(dt, "%Y-%m-%d %H:%M");
            s += "      " + std::string(n) + (d ? "" : "   " + hsize((double)sz)) + "   " + ds;
            g_free(ds); g_date_time_unref(dt); g_free(n);
        }
    } else if (sel.size() > 1) s += "      " + std::to_string(sel.size()) + " selected";
    return s;
}
