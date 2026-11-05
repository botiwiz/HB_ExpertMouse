// Thumbnails for the browser: reads/writes the shared freedesktop cache
// (~/.cache/thumbnails/large, 256 px), so thumbnails made by file managers (videos, PDFs...)
// are reused and ours are shared with them. Generated on a background worker pool.
#pragma once
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <functional>
#include <string>
#include <vector>

constexpr int kThumbMax = 256;

// Cached thumbnail if up to date, else (when can_generate) a new one, saved to the cache.
// Returns a new reference or nullptr. Thread-safe.
GdkPixbuf* thumb_get(const std::string& path, bool can_generate);

// Background generation. Results arrive on the GTK main thread through 'done'.
// 'gen' identifies the folder listing the request belongs to.
void thumbs_start(std::function<void(int gen, const std::string& path, GdkPixbuf* pb)> done);
void thumbs_request(int gen, const std::vector<std::pair<std::string, bool>>& paths_can_generate);
void thumbs_prioritize(const std::vector<std::string>& paths);   // e.g. the visible items
