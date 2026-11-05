// File browser: folder tree | path bar + thumbnail grid of ALL files in the folder.
#pragma once
#include <gtk/gtk.h>
#include <functional>
#include <string>
#include <vector>

struct BrowserHost {
    std::function<void(const std::string& path)> activate;       // double-click / Enter on a file
    std::function<void(GdkEvent* ev)> context_menu;              // right-click
    std::function<void()> changed;                               // selection or folder changed
};

enum BrowserSort { SORT_NAME, SORT_DATE, SORT_SIZE, SORT_TYPE };

GtkWidget* browser_create(BrowserHost host);
bool browser_set_folder(const std::string& dir, const std::string& select = "");
const std::string& browser_folder();
std::vector<std::string> browser_selected();
void browser_refresh();                      // re-list, keep selection
void browser_go_up();
void browser_focus();
void browser_thumb_step(int dir);            // +1 bigger / -1 smaller
int browser_thumb_size();
void browser_set_thumb_size(int px);
void browser_set_options(int sort, bool reverse, bool hidden);
int browser_sort();
bool browser_reverse();
bool browser_hidden();
int browser_pane_pos();
void browser_set_pane_pos(int px);
std::string browser_status();
