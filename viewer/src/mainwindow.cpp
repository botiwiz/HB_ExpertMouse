#include "mainwindow.h"
#include "imageview.h"
#ifdef HBV_HAVE_F3D
#include "modelview.h"
#endif

#include <KActionCollection>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <KFilePlacesModel>
#include <KFilePlacesView>
#include <KIO/ApplicationLauncherJob>
#include <KIO/CopyJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenFileManagerWindowJob>
#include <KIO/OpenUrlJob>
#include <KIO/RenameFileDialog>
#include <KParts/OpenUrlArguments>
#include <KParts/PartLoader>
#include <KParts/ReadOnlyPart>
#include <KPluginMetaData>
#include <KPropertiesDialog>
#include <KUrlNavigator>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCollator>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageWriter>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <thread>

static const QStringList kGroups = {"File", "Navigate", "Browser", "View", "Image", "3D", "Favorites", "Help", "Dolphin"};

static bool viewable(const QString& p) {
    std::string e = lower_ext(p.toStdString());
    return is_image_ext(e) || is_3d_ext(e);
}
static QString humanSize(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"}; int k = 0;
    while (b >= 1024 && k < 4) { b /= 1024; k++; }
    return k ? QString::number(b, 'f', 1) + " " + u[k] : QString::number((qint64)b) + " B";
}

// ---------------------------------------------------------------- favorites-aware menu
// Right-click a command in any of our menus to pin it to / unpin it from the toolbar.
class FavMenu : public QMenu {
public:
    std::function<QString(QAction*)> idOf;
    std::function<void(QAction*, const QString&)> toggle;
    using QMenu::QMenu;
protected:
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::RightButton) {
            QAction* a = actionAt(e->position().toPoint());
            QString id = a && idOf ? idOf(a) : QString();
            if (!id.isEmpty()) { toggle(a, id); return; }       // keep the menu open
        }
        QMenu::mouseReleaseEvent(e);
    }
};

// ---------------------------------------------------------------- construction
MainWindow::MainWindow() {
    setWindowTitle("HB Viewer");
    setWindowIcon(QIcon::fromTheme("hb_viewer", QIcon::fromTheme("image-x-generic")));

    // settings: next to the AppImage if writable, else ~/.config/hb_viewer/
    QString dir;
    if (QByteArray ai = qgetenv("APPIMAGE"); !ai.isEmpty() && QFileInfo(QFileInfo(QString::fromLocal8Bit(ai)).absolutePath()).isWritable())
        dir = QFileInfo(QString::fromLocal8Bit(ai)).absolutePath();
    else {
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/hb_viewer";
        QDir().mkpath(dir);
    }
    ini_ = new QSettings(dir + "/hb_viewer.ini", QSettings::IniFormat, this);

    stack_ = new QStackedWidget(this);
    setCentralWidget(stack_);

    // browser page: Places | (breadcrumb + Dolphin view)
    browserPage_ = new QWidget;
    auto* split = new QSplitter(Qt::Horizontal, browserPage_);
    auto* placesModel = new KFilePlacesModel(this);
    places_ = new KFilePlacesView(split);
    places_->setModel(placesModel);
    auto* right = new QWidget(split);
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    nav_ = new KUrlNavigator(placesModel, QUrl::fromLocalFile(QDir::homePath()), right);
    rl->addWidget(nav_);
    split->setStretchFactor(1, 1);
    split->setSizes({200, 1000});
    auto* bl = new QVBoxLayout(browserPage_);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addWidget(split);
    stack_->addWidget(browserPage_);

    loadDolphin();
    if (part_) rl->addWidget(part_->widget(), 1);
    else {
        auto* l = new QLabel("Dolphin's browser component could not be loaded.\n" + partError_ +
                             "\n\nInstall the 'dolphin' package (Plasma 6). The image viewer still works.", right);
        l->setAlignment(Qt::AlignCenter);
        l->setWordWrap(true);
        rl->addWidget(l, 1);
    }
    connect(nav_, &KUrlNavigator::urlChanged, this, [this](const QUrl& u) {
        if (part_ && part_->url().adjusted(QUrl::StripTrailingSlash) != u.adjusted(QUrl::StripTrailingSlash)) part_->openUrl(u);
        places_->setUrl(u);
        updateTitle();
    });
    connect(places_, &KFilePlacesView::placeActivated, this, [this](const QUrl& u) { navigateTo(u); });

    view_ = new ImageView;
    stack_->addWidget(view_);
    connect(view_, &ImageView::statusChanged, this, &MainWindow::updateStatus);
    connect(view_, &ImageView::navigate, this, [this](int d) { go(d); });
    connect(view_, &ImageView::contextMenuRequested, this, &MainWindow::showCommandMenu);
    connect(view_, &ImageView::doubleClicked, this, &MainWindow::toggleFullscreen);
#ifdef HBV_HAVE_F3D
    model_ = new ModelView;
    stack_->addWidget(model_);
    connect(model_, &ModelView::contextMenuRequested, this, &MainWindow::showCommandMenu);
    connect(model_, &ModelView::loaded, this, [this](bool) { updateStatus(); });
#endif

    tb_ = addToolBar("Favorites");
    tb_->setObjectName("favorites");
    tb_->setMovable(false);
    tb_->setContextMenuPolicy(Qt::CustomContextMenu);
    tb_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(tb_, &QToolBar::customContextMenuRequested, this, [this](const QPoint& pos) {
        QAction* a = tb_->actionAt(pos);
        QString id = a ? commandIdFor(a) : QString();
        QMenu m(this);
        if (!id.isEmpty()) {
            m.addAction(QIcon::fromTheme("list-remove"), "Remove from favorites", this, [this, id] { toggleFavorite(id); });
            m.addAction(QIcon::fromTheme("go-previous"), "Move left", this, [this, id] {
                int i = favs_.indexOf(id); if (i > 0) { favs_.swapItemsAt(i, i - 1); rebuildToolbar(); saveSettings(); } });
            m.addAction(QIcon::fromTheme("go-next"), "Move right", this, [this, id] {
                int i = favs_.indexOf(id); if (i >= 0 && i + 1 < favs_.size()) { favs_.swapItemsAt(i, i + 1); rebuildToolbar(); saveSettings(); } });
            m.addSeparator();
        }
        m.addAction(QIcon::fromTheme("bookmarks"), "Customize favorites...", this, &MainWindow::customizeFavorites);
        m.exec(tb_->mapToGlobal(pos));
    });

    statusL_ = new QLabel;
    statusR_ = new QLabel;
    statusL_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addWidget(statusL_, 1);
    statusBar()->addPermanentWidget(statusR_);

    slideshow_ = new QTimer(this);
    connect(slideshow_, &QTimer::timeout, this, [this] { go(1); });
    progress_ = new QTimer(this);
    progress_->setInterval(100);
    connect(progress_, &QTimer::timeout, this, [this] { if (jobs_.count(want_)) updateStatus(); else progress_->stop(); });

    loadSettings();
    buildCommands();
    addDolphinCommands();
    rebuildToolbar();
    setAcceptDrops(true);
    stack_->setCurrentWidget(browserPage_);
    updateStatus();
}

MainWindow::~MainWindow() {
    for (auto& j : jobs_) j.second.ctl->cancel = true;
}

void MainWindow::loadDolphin() {
    KPluginMetaData md = KPluginMetaData::findPluginById(QStringLiteral("kf6/parts"), QStringLiteral("dolphinpart"));
    if (!md.isValid()) {
        for (const KPluginMetaData& m : KParts::PartLoader::partsForMimeType(QStringLiteral("inode/directory")))
            if (m.pluginId().contains("dolphin")) { md = m; break; }
    }
    if (!md.isValid()) { partError_ = "Plugin 'dolphinpart' not found."; return; }
    auto res = KParts::PartLoader::instantiatePart<KParts::ReadOnlyPart>(md, this, this);
    if (!res.plugin) { partError_ = res.errorString; return; }
    part_ = res.plugin;
    ext_ = KParts::NavigationExtension::childObject(part_);
    // the part's own shortcuts (F2, Del, Ctrl+C...) work while its view has focus
    for (QAction* a : part_->actionCollection()->actions()) {
        if (!a) continue;
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        part_->widget()->addAction(a);
    }
    if (ext_) {
        connect(ext_, &KParts::NavigationExtension::openUrlRequest, this,
                [this](const QUrl& u, const KParts::OpenUrlArguments& a) { onOpenUrlRequest(u, a); });
        connect(ext_, &KParts::NavigationExtension::openUrlRequestDelayed, this,
                [this](const QUrl& u, const KParts::OpenUrlArguments& a) { onOpenUrlRequest(u, a); });
        connect(ext_, qOverload<const QPoint&, const KFileItemList&, const KParts::OpenUrlArguments&,
                                KParts::NavigationExtension::PopupFlags, const KParts::NavigationExtension::ActionGroupMap&>(
                          &KParts::NavigationExtension::popupMenu),
                this, [this](const QPoint& g, const KFileItemList& items, const KParts::OpenUrlArguments&,
                             KParts::NavigationExtension::PopupFlags f, const KParts::NavigationExtension::ActionGroupMap& m) {
                    browserPopup(g, items, f, m); });
        connect(ext_, &KParts::NavigationExtension::selectionInfo, this, [this](const KFileItemList& l) { selection_ = l; updateStatus(); });
        connect(ext_, &KParts::NavigationExtension::setLocationBarUrl, this, [this](const QString& s) {
            QUrl u = QUrl::fromUserInput(s);
            if (u.isValid() && u != nav_->locationUrl()) nav_->setLocationUrl(u);
        });
    }
    connect(part_, &KParts::Part::setStatusBarText, this, [this](const QString& s) { partStatus_ = s; updateStatus(); });
    connect(part_, &KParts::ReadOnlyPart::completed, this, [this] { updateStatus(); });
}

void MainWindow::triggerPartAction(const char* name) {
    if (!part_) return;
    if (QAction* a = part_->actionCollection()->action(QString::fromLatin1(name))) a->trigger();
}

// ---------------------------------------------------------------- commands
QAction* MainWindow::addCmd(const QString& id, const QString& label, const QString& group, const QString& icon,
                            const QList<QKeySequence>& keys, std::function<void()> fn, QWidget* scope,
                            std::function<bool()> checked) {
    auto* a = new QAction(QIcon::fromTheme(icon), label, this);
    a->setObjectName(id);
    a->setShortcuts(keys);
    if (checked) {
        a->setCheckable(true);
        a->setChecked(checked());
        connect(a, &QAction::triggered, this, [this, a, fn, checked] { fn(); a->setChecked(checked()); saveSettings(); });
    } else connect(a, &QAction::triggered, this, [fn] { fn(); });
    // shortcut scope: viewer-only commands are active only while the viewer has focus
    if (scope) { a->setShortcutContext(Qt::WidgetWithChildrenShortcut); scope->addAction(a); }
    else { a->setShortcutContext(Qt::WindowShortcut); addAction(a); }
    cmds_.append({id, label, group, a});
    return a;
}

void MainWindow::buildCommands() {
    using K = QKeySequence;
    QWidget* V = view_;              // viewer-only scope
    QWidget* B = browserPage_;       // browser-only scope
    // File
    addCmd("open", "Open...", "File", "document-open", {K::Open}, [this] { cmdOpen(); }, nullptr);
    addCmd("reload", "Reload", "File", "view-refresh", {K(Qt::Key_F5)}, [this] {
        if (!want_.isEmpty()) { cache_.erase(want_); view_->clear(); curPath_.clear(); openInViewer(want_); } }, V);
    addCmd("save_as", "Save as...", "File", "document-save-as", {K::Save}, [this] { cmdSaveAs(); }, V);
    addCmd("open_with", "Open with...", "File", "system-run", {K(Qt::Key_E)}, [this] { cmdOpenWith(); }, V);
    addCmd("show_folder", "Show in browser", "File", "folder-open", {K(Qt::CTRL | Qt::SHIFT | Qt::Key_O)}, [this] {
        if (!want_.isEmpty()) showBrowser(QUrl::fromLocalFile(QFileInfo(want_).absolutePath()), QUrl::fromLocalFile(want_)); }, nullptr);
    addCmd("open_dolphin", "Show in Dolphin (new window)", "File", "system-file-manager", {}, [this] {
        QList<QUrl> t = targets(); if (!t.isEmpty()) KIO::highlightInFileManager(t); }, nullptr);
    addCmd("copy_image", "Copy image", "File", "edit-copy", {K::Copy}, [this] {
        QImage q = view_->transformedImage(); if (!q.isNull()) QApplication::clipboard()->setImage(q); }, V);
    addCmd("copy_path", "Copy file path", "File", "edit-copy-path", {K(Qt::CTRL | Qt::SHIFT | Qt::Key_C)}, [this] {
        QString p = targetPath(); if (!p.isEmpty()) QApplication::clipboard()->setText(p); }, nullptr);
    addCmd("rename", "Rename...", "File", "edit-rename", {K(Qt::Key_F2)}, [this] { cmdRename(); }, V);
    addCmd("trash", "Move to trash", "File", "user-trash", {K(Qt::Key_Delete)}, [this] { cmdTrash(); }, V);
    addCmd("info", "File info / properties", "File", "document-properties", {K(Qt::Key_I)}, [this] { cmdInfo(); }, V);
    addCmd("quit", "Quit", "File", "application-exit", {K::Quit}, [this] { close(); }, nullptr);
    // Navigate (viewer)
    addCmd("next", "Next image", "Navigate", "go-next", {K(Qt::Key_Right), K(Qt::Key_PageDown), K(Qt::Key_Space)}, [this] { go(1); }, V);
    addCmd("prev", "Previous image", "Navigate", "go-previous", {K(Qt::Key_Left), K(Qt::Key_PageUp), K(Qt::Key_Backspace)}, [this] { go(-1); }, V);
    addCmd("first", "First image", "Navigate", "go-first", {K(Qt::Key_Home)}, [this] { go(1, true); }, V);
    addCmd("last", "Last image", "Navigate", "go-last", {K(Qt::Key_End)}, [this] { go(-1, true); }, V);
    addCmd("slideshow", "Slideshow", "Navigate", "media-playback-start", {K(Qt::Key_S)}, [this] {
        if (slideshow_->isActive()) slideshow_->stop(); else slideshow_->start(slideshowMs_); updateStatus(); },
        V, [this] { return slideshow_->isActive(); });
    addCmd("loop", "Loop at end of folder", "Navigate", "media-playlist-repeat", {}, [this] { loopDir_ = !loopDir_; }, nullptr,
           [this] { return loopDir_; });
    addCmd("back_browser", "Back to browser", "Navigate", "go-up", {K(Qt::Key_Escape)}, [this] {
        if (isFullScreen()) toggleFullscreen();
        else if (slideshow_->isActive()) { slideshow_->stop(); updateStatus(); }
        else if (!want_.isEmpty()) showBrowser(QUrl::fromLocalFile(QFileInfo(want_).absolutePath()), QUrl::fromLocalFile(want_));
        else setViewerMode(false); }, V);
    // Browser
    addCmd("browser", "Browser / viewer", "Browser", "view-list-icons", {K(Qt::Key_Tab)}, [this] {
        if (!viewer_) {
            QString t = targetPath();
            if (!t.isEmpty() && viewable(t)) viewFile(t);
            else if (!want_.isEmpty()) setViewerMode(true);
        } else setViewerMode(false); }, nullptr, [this] { return !viewer_; });
    addCmd("go_back", "Back", "Browser", "go-previous", {K(Qt::ALT | Qt::Key_Left), K(Qt::Key_Back)}, [this] { nav_->goBack(); }, B);
    addCmd("go_forward", "Forward", "Browser", "go-next", {K(Qt::ALT | Qt::Key_Right), K(Qt::Key_Forward)}, [this] { nav_->goForward(); }, B);
    addCmd("go_up", "Parent folder", "Browser", "go-up", {K(Qt::ALT | Qt::Key_Up), K(Qt::Key_Backspace)}, [this] { nav_->goUp(); }, B);
    addCmd("go_home", "Home folder", "Browser", "go-home", {K(Qt::ALT | Qt::Key_Home)}, [this] { navigateTo(QUrl::fromLocalFile(QDir::homePath())); }, B);
    addCmd("edit_location", "Edit location", "Browser", "document-edit", {K(Qt::CTRL | Qt::Key_L)}, [this] {
        nav_->setUrlEditable(true); nav_->setFocus(); }, B);
    addCmd("places", "Show Places panel", "Browser", "compass", {K(Qt::Key_F9)}, [this] { places_->setVisible(!places_->isVisible()); },
           nullptr, [this] { return places_->isVisible(); });
    // View
    addCmd("zoom_in", "Zoom in", "View", "zoom-in", {K::ZoomIn}, [this] {
        if (viewer_) view_->zoomStep(1); else triggerPartAction("view_zoom_in"); }, nullptr);
    addCmd("zoom_out", "Zoom out", "View", "zoom-out", {K::ZoomOut}, [this] {
        if (viewer_) view_->zoomStep(-1); else triggerPartAction("view_zoom_out"); }, nullptr);
    addCmd("zoom_100", "Actual pixels (100%)", "View", "zoom-original", {K(Qt::Key_1), K(Qt::Key_Asterisk)}, [this] { view_->actualPixels(); }, V);
    addCmd("zoom_fit", "Fit to window", "View", "zoom-fit-best", {K(Qt::Key_0), K(Qt::Key_Slash)}, [this] { view_->fit(true); }, V);
    addCmd("zoom_fit_width", "Fit width", "View", "zoom-fit-width", {K(Qt::Key_W)}, [this] { view_->fitWidth(); }, V);
    addCmd("fit_on_open", "Fit large images on open", "View", "zoom-fit-best", {}, [this] { view_->fitOnOpen = !view_->fitOnOpen; },
           nullptr, [this] { return view_->fitOnOpen; });
    addCmd("fullscreen", "Full screen", "View", "view-fullscreen", {K(Qt::Key_F11)},
           [this] { toggleFullscreen(); }, nullptr, [this] { return isFullScreen(); });
    addCmd("background", "Cycle background", "View", "color-management", {K(Qt::Key_B)}, [this] {
        view_->bg = (view_->bg + 1) % ImageView::BgCount; view_->update(); saveSettings(); }, V);
    addCmd("smooth", "Smooth when zoomed in", "View", "blurfx", {K(Qt::Key_P)}, [this] { view_->smooth = !view_->smooth; view_->update(); },
           V, [this] { return view_->smooth; });
    addCmd("tiled", "Tiled view (seamless check)", "View", "view-grid", {K(Qt::Key_T)}, [this] { view_->tiled = !view_->tiled; view_->update(); },
           V, [this] { return view_->tiled; });
    addCmd("wheel_zoom", "Mouse wheel zooms (off = navigates)", "View", "input-mouse", {K(Qt::Key_M)},
           [this] { view_->wheelZoom = !view_->wheelZoom; }, V, [this] { return view_->wheelZoom; });
    addCmd("toolbar", "Show toolbar", "View", "configure-toolbars", {K(Qt::CTRL | Qt::Key_T)}, [this] { tb_->setVisible(!tb_->isVisible()); },
           nullptr, [this] { return tb_->isVisible(); });
    addCmd("statusbar", "Show status bar", "View", "show-menu", {K(Qt::CTRL | Qt::Key_B)}, [this] { statusBar()->setVisible(!statusBar()->isVisible()); },
           nullptr, [this] { return statusBar()->isVisible(); });
    addCmd("on_top", "Always on top", "View", "window-keep-above", {}, [this] {
        setWindowFlag(Qt::WindowStaysOnTopHint, !(windowFlags() & Qt::WindowStaysOnTopHint)); show(); },
        nullptr, [this] { return bool(windowFlags() & Qt::WindowStaysOnTopHint); });
    // Image
    addCmd("rotate_left", "Rotate left", "Image", "object-rotate-left", {K(Qt::Key_L)}, [this] { view_->rotate(-1); }, V);
    addCmd("rotate_right", "Rotate right", "Image", "object-rotate-right", {K(Qt::Key_R)}, [this] { view_->rotate(1); }, V);
    addCmd("flip_h", "Flip horizontal", "Image", "object-flip-horizontal", {K(Qt::Key_H)}, [this] { view_->flipH(); }, V);
    addCmd("flip_v", "Flip vertical", "Image", "object-flip-vertical", {K(Qt::Key_V)}, [this] { view_->flipV(); }, V);
    addCmd("reset_transform", "Reset rotation / flip", "Image", "edit-undo", {K(Qt::CTRL | Qt::Key_R)}, [this] { view_->resetTransform(); }, V);
    // 3D
    addCmd("f3d", "Open in f3d app (external window)", "3D", "draw-cuboid", {K(Qt::Key_D)}, [this] { cmdF3d(); }, V);
#ifdef HBV_HAVE_F3D
    {
        QWidget* M = model_;                         // 3D-view scope; other keys go to f3d itself
        auto f3dCmd = [this](const QString& id, const QString& label, const QString& icon, const QString& cmd) {
            addCmd(id, label, "3D", icon, {}, [this, cmd] { if (model_) model_->command(cmd); }, nullptr);
        };
        f3dCmd("f3d_reset", "Reset camera", "zoom-fit-best", "reset_camera");
        f3dCmd("f3d_front", "View: front", "go-down", "set_camera front");
        f3dCmd("f3d_top", "View: top", "go-top", "set_camera top");
        f3dCmd("f3d_right", "View: right", "go-next", "set_camera right");
        f3dCmd("f3d_iso", "View: isometric", "draw-cuboid", "set_camera isometric");
        f3dCmd("f3d_grid", "Toggle grid", "view-grid", "toggle render.grid.enable");
        f3dCmd("f3d_edges", "Toggle edges (wireframe overlay)", "draw-polyline", "toggle render.show_edges");
        f3dCmd("f3d_axis", "Toggle axis widget", "transform-move", "toggle ui.axis");
        f3dCmd("f3d_ortho", "Toggle orthographic camera", "draw-rectangle", "toggle scene.camera.orthographic");
        f3dCmd("f3d_anim", "Play / pause animation", "media-playback-start", "toggle_animation");
        f3dCmd("f3d_fps", "Toggle FPS counter", "speedometer", "toggle ui.fps");
        f3dCmd("f3d_metadata", "Toggle model info", "documentinfo", "toggle ui.metadata");
        f3dCmd("f3d_cheatsheet", "f3d keyboard cheatsheet", "help-keybord-shortcuts", "toggle ui.cheatsheet");
        // keys of ours that must win inside the 3D view
        for (const Command& c : cmds_) {
            auto scoped = [&](const QList<QKeySequence>& keys) {
                auto* a = new QAction(this);
                a->setShortcuts(keys);
                a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
                QAction* real = c.action;
                connect(a, &QAction::triggered, real, &QAction::trigger);
                M->addAction(a);
            };
            if (c.id == "next") scoped({K(Qt::Key_PageDown), K(Qt::Key_Right)});
            else if (c.id == "prev") scoped({K(Qt::Key_PageUp), K(Qt::Key_Left)});
            else if (c.id == "back_browser") scoped({K(Qt::Key_Escape)});
            else if (c.id == "f3d") scoped({K(Qt::CTRL | Qt::Key_D)});
        }
    }
#endif
    // single-key extras that only make sense in the viewer (Dolphin needs these keys for typing)
    auto alias = [this](const QString& id, const QList<QKeySequence>& keys) {
        for (const Command& c : cmds_)
            if (c.id == id) {
                auto* a = new QAction(this);
                a->setShortcuts(keys);
                a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
                QAction* real = c.action;
                connect(a, &QAction::triggered, real, &QAction::trigger);
                view_->addAction(a);
            }
    };
    alias("zoom_in", {K(Qt::Key_Plus), K(Qt::Key_Equal)});
    alias("zoom_out", {K(Qt::Key_Minus)});
    alias("fullscreen", {K(Qt::Key_F), K(Qt::Key_Return), K(Qt::Key_Enter)});
    // Favorites / Help
    addCmd("customize_favorites", "Customize favorites...", "Favorites", "bookmarks", {}, [this] { customizeFavorites(); }, nullptr);
    addCmd("show_menu", "All commands menu", "Favorites", "application-menu", {K(Qt::Key_Menu)}, [this] {
        showCommandMenu(mapToGlobal(QPoint(width() / 2, height() / 3))); }, nullptr);
    addCmd("help", "Commands and shortcuts", "Help", "help-contents", {K::HelpContents}, [this] { shortcutsHelp(); }, nullptr);
    addCmd("about", "About HB Viewer", "Help", "help-about", {}, [this] {
        QMessageBox::about(this, "About HB Viewer", "<b>HB Viewer</b> " + QApplication::applicationVersion() +
            "<p>Image viewer for huge PSD/PSB and common formats, with Dolphin as its file browser.</p>"); }, nullptr);
}

// The part's own actions (view modes, sorting, hidden files, zoom, edit actions...) become
// commands too, so they show up in the menu and can be pinned to the toolbar.
void MainWindow::addDolphinCommands() {
    if (!part_) return;
    // Dolphin's own shortcuts win in the browser: drop ours (global/browser scope) that collide
    QSet<QKeySequence> partKeys;
    for (QAction* a : part_->actionCollection()->actions()) for (const QKeySequence& k : a->shortcuts()) partKeys.insert(k);
    for (const Command& c : cmds_) {
        if (view_->actions().contains(c.action)) continue;              // viewer-only scope: no conflict
        QList<QKeySequence> keep;
        for (const QKeySequence& k : c.action->shortcuts()) if (!partKeys.contains(k)) keep.append(k);
        c.action->setShortcuts(keep);
    }
    for (QAction* a : part_->actionCollection()->actions()) {
        if (!a || a->isSeparator() || a->menu() || a->text().isEmpty() || a->objectName().isEmpty()) continue;
        QString label = a->text().remove('&');
        cmds_.append({"dolphin:" + a->objectName(), label, "Dolphin", a});
    }
}

QString MainWindow::commandIdFor(QAction* a) const {
    if (!a) return {};
    QVariant v = a->property("hbv_cmd");
    if (v.isValid()) return v.toString();
    for (const Command& c : cmds_) if (c.action == a) return c.id;
    return {};
}

QString MainWindow::menuText(const Command& c) const {
    QString t = (isFavorite(c.id) ? QStringLiteral("★ ") : QString()) + c.label;
    QKeySequence k = c.action->shortcut();
    if (!k.isEmpty()) t += '\t' + k.toString(QKeySequence::NativeText);
    return t;
}

QMenu* MainWindow::buildCommandMenu(QWidget* parent, QMenu* into) {
    auto setup = [this](FavMenu* m) {
        m->idOf = [this](QAction* a) { return commandIdFor(a); };
        m->toggle = [this](QAction* a, const QString& id) {
            toggleFavorite(id);
            for (const Command& c : cmds_) if (c.id == id) { a->setText(menuText(c)); break; }
        };
    };
    FavMenu* root = into ? nullptr : new FavMenu(parent);
    if (root) setup(root);
    QMenu* top = into ? into : root;
    for (const QString& g : kGroups) {
        QList<const Command*> list;
        for (const Command& c : cmds_) if (c.group == g) list.append(&c);
        if (list.isEmpty()) continue;
        auto* sub = new FavMenu(g, top);
        setup(sub);
        top->addMenu(sub);
        for (const Command* c : list) {
            // wrapper so the menu can show a star without changing the real action's text
            QAction* w = sub->addAction(c->action->icon(), menuText(*c));
            w->setProperty("hbv_cmd", c->id);
            w->setEnabled(c->action->isEnabled());
            if (c->action->isCheckable()) { w->setCheckable(true); w->setChecked(c->action->isChecked()); }
            QAction* real = c->action;
            connect(w, &QAction::triggered, real, [real] { QTimer::singleShot(0, real, &QAction::trigger); });
        }
    }
    top->addSeparator();
    QAction* hint = top->addAction(QStringLiteral("★ Right-click a command to pin it to the toolbar"));
    hint->setEnabled(false);
    return top;
}

void MainWindow::showCommandMenu(QPoint global) {
    QMenu* m = buildCommandMenu(this);
    m->setAttribute(Qt::WA_DeleteOnClose);
    m->popup(global);
}

void MainWindow::browserPopup(const QPoint& global, const KFileItemList& items,
                              KParts::NavigationExtension::PopupFlags flags,
                              const KParts::NavigationExtension::ActionGroupMap& groups) {
    auto* menu = new FavMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->idOf = [this](QAction* a) { return commandIdFor(a); };
    menu->toggle = [this](QAction*, const QString& id) { toggleFavorite(id); };
    // right-click on empty space: Dolphin passes the folder itself as the only item
    bool isBackground = items.isEmpty() || (items.size() == 1 && part_ &&
        items.first().url().adjusted(QUrl::StripTrailingSlash) == part_->url().adjusted(QUrl::StripTrailingSlash));

    if (items.size() == 1 && items.first().isLocalFile() && !items.first().isDir() && viewable(items.first().localPath())) {
        QString p = items.first().localPath();
        QAction* v = menu->addAction(QIcon::fromTheme("image-x-generic"), "View in HB Viewer", this, [this, p] { viewFile(p); });
        menu->setDefaultAction(v);
        menu->addSeparator();
    }
    if (isBackground) {
        for (const char* n : {"go_back", "go_forward", "go_up"})
            for (const Command& c : cmds_) if (c.id == n) menu->addAction(c.action);
        menu->addSeparator();
    }
    auto* fia = new KFileItemActions(menu);
    fia->setParentWidget(this);
    fia->setItemListProperties(KFileItemListProperties(items));
    if (!isBackground) { fia->insertOpenWithActionsTo(nullptr, menu, QStringList()); menu->addSeparator(); }
    // the part's own groups (cut/copy/paste/rename/trash/new...), in Dolphin's order
    for (const QString& key : {QStringLiteral("newmenu"), QStringLiteral("editactions"), QStringLiteral("linkactions"),
                               QStringLiteral("partactions"), QStringLiteral("preview")}) {
        const QList<QAction*> acts = groups.value(key);
        if (acts.isEmpty()) continue;
        menu->addActions(acts);
        menu->addSeparator();
    }
    fia->addActionsTo(menu);            // service menus ("Actions", "Compress", ...)
    menu->addSeparator();
    QMenu* hb = menu->addMenu(QIcon::fromTheme("hb_viewer", QIcon::fromTheme("image-x-generic")), "HB Viewer commands");
    buildCommandMenu(this, hb);
    if (!items.isEmpty()) {
        menu->addSeparator();
        menu->addAction(QIcon::fromTheme("document-properties"), "Properties", this, [this, items] {
            KPropertiesDialog::showDialog(items, this, false); });
    }
    menu->popup(global);
}

void MainWindow::toggleFavorite(const QString& id) {
    if (favs_.contains(id)) favs_.removeAll(id); else favs_.append(id);
    rebuildToolbar();
    saveSettings();
}

void MainWindow::rebuildToolbar() {
    tb_->clear();
    for (const QString& id : favs_)
        for (const Command& c : cmds_)
            if (c.id == id) {
                tb_->addAction(c.action);
                if (auto* b = qobject_cast<QToolButton*>(tb_->widgetForAction(c.action))) {
                    if (c.action->icon().isNull()) b->setToolButtonStyle(Qt::ToolButtonTextOnly);
                    QString tip = c.label;
                    if (!c.action->shortcut().isEmpty()) tip += "  (" + c.action->shortcut().toString(QKeySequence::NativeText) + ")";
                    b->setToolTip(tip + "\nRight-click for options");
                }
                break;
            }
    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb_->addWidget(spacer);
    QAction* all = tb_->addAction(QIcon::fromTheme("application-menu"), "All commands");
    all->setToolTip("All commands (right-click a command in the menu to pin it here)");
    connect(all, &QAction::triggered, this, [this] {
        QWidget* w = tb_->widgetForAction(tb_->actions().last());
        showCommandMenu(w ? w->mapToGlobal(QPoint(0, w->height())) : QCursor::pos());
    });
}

void MainWindow::customizeFavorites() {
    QDialog d(this);
    d.setWindowTitle("Customize favorites");
    auto* lay = new QVBoxLayout(&d);
    lay->addWidget(new QLabel("Tick the commands to show on the toolbar. Drag rows to change their order."));
    auto* list = new QListWidget;
    list->setDragDropMode(QAbstractItemView::InternalMove);
    auto addRow = [list](const Command& c, bool on) {
        auto* it = new QListWidgetItem(c.action->icon(), c.label + "   (" + c.group + ")");
        it->setData(Qt::UserRole, c.id);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
        it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        list->addItem(it);
    };
    for (const QString& id : favs_) for (const Command& c : cmds_) if (c.id == id) { addRow(c, true); break; }
    for (const Command& c : cmds_) if (!favs_.contains(c.id)) addRow(c, false);
    lay->addWidget(list);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    lay->addWidget(bb);
    d.resize(520, 600);
    if (d.exec() != QDialog::Accepted) return;
    favs_.clear();
    for (int i = 0; i < list->count(); i++)
        if (list->item(i)->checkState() == Qt::Checked) favs_.append(list->item(i)->data(Qt::UserRole).toString());
    rebuildToolbar();
    saveSettings();
}

// ---------------------------------------------------------------- settings
void MainWindow::loadSettings() {
    QSettings& s = *ini_;
    view_->bg = std::clamp(s.value("view/background", 0).toInt(), 0, ImageView::BgCount - 1);
    view_->smooth = s.value("view/smooth_zoom_in", true).toBool();
    view_->wheelZoom = s.value("view/wheel_zooms", true).toBool();
    view_->fitOnOpen = s.value("view/fit_large_images", true).toBool();
    loopDir_ = s.value("view/loop_folder", true).toBool();
    confirmDelete_ = s.value("view/confirm_delete", true).toBool();
    slideshowMs_ = std::max(200, s.value("view/slideshow_ms", 3000).toInt());
    int mp = s.value("view/max_megapixels", 0).toInt();       // 0 = auto (about a third of RAM)
    if (mp > 0) budget_ = (uint64_t)mp * 1000000;
    else {
        uint64_t mem = 8ull << 30;
        if (FILE* f = fopen("/proc/meminfo", "r")) { unsigned long long kb; if (fscanf(f, "MemTotal: %llu kB", &kb) == 1) mem = kb * 1024; fclose(f); }
        budget_ = std::clamp<uint64_t>(mem / 3 / 6, 64000000ull, 2000000000ull);
    }
    favs_ = s.value("favorites/commands", QStringList{"browser", "open", "dolphin:go_up", "prev", "next", "zoom_fit", "zoom_100",
                                                      "zoom_in", "zoom_out", "rotate_left", "rotate_right", "fullscreen", "info"}).toStringList();
    if (s.contains("window/geometry")) restoreGeometry(s.value("window/geometry").toByteArray());
    else resize(1280, 820);
    tb_->setVisible(s.value("window/toolbar", true).toBool());
    statusBar()->setVisible(s.value("window/statusbar", true).toBool());
    places_->setVisible(s.value("browser/places", true).toBool());
}

void MainWindow::saveSettings() {
    QSettings& s = *ini_;
    s.setValue("view/background", view_->bg);
    s.setValue("view/smooth_zoom_in", view_->smooth);
    s.setValue("view/wheel_zooms", view_->wheelZoom);
    s.setValue("view/fit_large_images", view_->fitOnOpen);
    s.setValue("view/loop_folder", loopDir_);
    s.setValue("view/confirm_delete", confirmDelete_);
    s.setValue("view/slideshow_ms", slideshowMs_);
    if (!s.contains("view/max_megapixels")) s.setValue("view/max_megapixels", 0);
    s.setValue("favorites/commands", favs_);
    if (!isFullScreen()) s.setValue("window/geometry", saveGeometry());
    s.setValue("window/toolbar", tb_->isVisible());
    s.setValue("window/statusbar", statusBar()->isVisible());
    s.setValue("browser/places", places_->isVisible());
    if (part_ && part_->url().isValid()) s.setValue("browser/folder", part_->url().toString());
}

QString MainWindow::lastFolder() const {
    QUrl u(ini_->value("browser/folder").toString());
    if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isDir()) return u.toLocalFile();
    if (u.isValid() && !u.isLocalFile() && !u.isEmpty()) return u.toString();
    return QDir::homePath();
}

void MainWindow::closeEvent(QCloseEvent* e) { saveSettings(); e->accept(); }

// ---------------------------------------------------------------- browser
void MainWindow::navigateTo(const QUrl& url) {
    nav_->setLocationUrl(url);                     // -> urlChanged -> part_->openUrl
    if (part_ && part_->url().adjusted(QUrl::StripTrailingSlash) != url.adjusted(QUrl::StripTrailingSlash)) part_->openUrl(url);
}

void MainWindow::showBrowser(const QUrl& folder, const QUrl& select) {
    setViewerMode(false);
    if (folder.isValid()) navigateTo(folder);
    // ask Dolphin to select the file we came from (DolphinPart slot; harmless if missing)
    if (part_ && select.isValid())
        QMetaObject::invokeMethod(part_, "setFilesToSelect", Qt::QueuedConnection, Q_ARG(QList<QUrl>, QList<QUrl>{select}));
    if (part_) part_->widget()->setFocus();
}

// Activation in Dolphin: folders navigate, pictures/3D open in our viewer,
// everything else gets normal file-manager behaviour (default application).
void MainWindow::onOpenUrlRequest(const QUrl& url, const KParts::OpenUrlArguments& args) {
    QString mime = args.mimeType();
    bool dir = mime == "inode/directory" || (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir());
    if (dir) { navigateTo(url); return; }
    if (url.isLocalFile() && viewable(url.toLocalFile())) { viewFile(url.toLocalFile()); return; }
    if (mime.isEmpty() && url.isLocalFile()) mime = QMimeDatabase().mimeTypeForFile(url.toLocalFile()).name();
    auto* job = new KIO::OpenUrlJob(url, mime);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
    job->setShowOpenOrExecuteDialog(true);
    job->start();
}

// ---------------------------------------------------------------- viewer
void MainWindow::setViewerMode(bool viewer) {
    viewer_ = viewer;
    if (viewer) {
        QWidget* page = showingModel_ && model_ ? (QWidget*)model_ : (QWidget*)view_;
        stack_->setCurrentWidget(page);
        page->setFocus();
    }
    else {
        slideshow_->stop();
        stack_->setCurrentWidget(browserPage_);
        if (part_) part_->widget()->setFocus();
    }
    for (const Command& c : cmds_) if (c.id == "browser") c.action->setChecked(!viewer);
    updateTitle();
    updateStatus();
}

void MainWindow::viewFile(const QString& path) {
    setViewerMode(true);
    openInViewer(path);
}

void MainWindow::openPath(const QString& path) {
    QFileInfo fi(path);
    if (fi.isDir()) { showBrowser(QUrl::fromLocalFile(fi.absoluteFilePath())); return; }
    if (!fi.exists()) {                                        // maybe a URL (smb:/, sftp:/ ...)
        QUrl u = QUrl::fromUserInput(path);
        if (u.isValid() && !u.isLocalFile()) { showBrowser(u); return; }
    }
    if (viewable(fi.absoluteFilePath())) { viewFile(fi.absoluteFilePath()); return; }
    showBrowser(QUrl::fromLocalFile(fi.absolutePath()), QUrl::fromLocalFile(fi.absoluteFilePath()));
}

void MainWindow::scanFolder(const QString& file) {
    QFileInfo fi(file);
    QDir d(fi.absolutePath());
    QStringList names = d.entryList(QDir::Files | QDir::NoDotAndDotDot);
    QCollator col; col.setNumericMode(true); col.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), [&](const QString& a, const QString& b) { return col.compare(a, b) < 0; });
    files_.clear();
    for (const QString& n : names) if (viewable(n)) files_.append(d.absoluteFilePath(n));
    idx_ = files_.indexOf(fi.absoluteFilePath());
    if (idx_ < 0) { files_.prepend(fi.absoluteFilePath()); idx_ = 0; }
}

void MainWindow::openInViewer(const QString& path) {
    if (files_.indexOf(path) < 0) { scanFolder(path); cache_.clear(); }
    else idx_ = files_.indexOf(path);
    want_ = path;
    for (auto& j : jobs_) if (j.first != path) j.second.ctl->cancel = true;     // drop stale work
    if (is_3d_ext(lower_ext(path.toStdString()))) {
        view_->clear();
        curPath_ = path;
        if (model_) {                                   // embedded f3d
            showingModel_ = true;
            if (viewer_) { stack_->setCurrentWidget((QWidget*)model_); ((QWidget*)model_)->setFocus(); }
#ifdef HBV_HAVE_F3D
            model_->load(path);
#endif
        } else
            view_->setMessage("3D model: " + QFileInfo(path).fileName() + "\nPress D to open it in f3d.\n(This build has no embedded 3D viewer.)");
        updateTitle(); updateStatus();
        return;
    }
    if (showingModel_) {                                // back to pictures: free the GPU scene
        showingModel_ = false;
#ifdef HBV_HAVE_F3D
        if (model_) model_->unload();
#endif
        if (viewer_) { stack_->setCurrentWidget(view_); view_->setFocus(); }
    }
    auto c = cache_.find(path);
    if (c != cache_.end()) { loadSecs_ = 0; showResult(path, c->second); return; }
    if (!jobs_.count(path)) startJob(path);
    if (curPath_ != path) view_->setMessage("Loading...");
    progress_->start();
    updateTitle(); updateStatus();
}

void MainWindow::go(int delta, bool absolute) {
    if (files_.isEmpty()) return;
    int n = files_.size(), i;
    if (absolute) i = delta < 0 ? n - 1 : 0;
    else {
        i = idx_ + delta;
        if (i < 0 || i >= n) { if (!loopDir_) return; i = (i % n + n) % n; }
    }
    idx_ = i;
    openInViewer(files_[i]);
}

void MainWindow::startJob(const QString& path) {
    auto ctl = std::make_shared<LoadCtl>();
    jobs_[path] = {ctl, std::chrono::steady_clock::now()};
    uint64_t budget = budget_;
    QPointer<MainWindow> self(this);
    std::thread([self, path, ctl, budget] {
        auto preview = [self, path, ctl](std::unique_ptr<LoadResult> p) {
            std::shared_ptr<LoadResult> sp(std::move(p));
            QMetaObject::invokeMethod(qApp, [self, path, ctl, sp] {
                if (!self || ctl->cancel || path != self->want_) return;
                if (self->curPath_ == path && self->view_->result() && !self->view_->result()->info.preview) return;
                self->showResult(path, sp);
            }, Qt::QueuedConnection);
        };
        std::shared_ptr<LoadResult> r(load_image(path.toStdString(), budget, *ctl, preview).release());
        QMetaObject::invokeMethod(qApp, [self, path, ctl, r] {
            if (!self) return;
            auto it = self->jobs_.find(path);
            double secs = 0;
            if (it != self->jobs_.end()) {
                secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - it->second.start).count();
                self->jobs_.erase(it);
            }
            if (ctl->cancel) return;
            if (r->img) self->cache_[path] = r;
            if (path == self->want_) { self->loadSecs_ = secs; self->showResult(path, r); }
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::showResult(const QString& path, std::shared_ptr<LoadResult> r) {
    bool upgrade = curPath_ == path && view_->result() && view_->result()->info.preview;
    curPath_ = path;
    bool preview = r->info.preview;
    view_->setResult(std::move(r), path, upgrade);
    updateTitle(); updateStatus();
    if (!preview) preloadNeighbours();
}

void MainWindow::preloadNeighbours() {
    QStringList keep = {curPath_};
    int n = files_.size();
    if (n > 1) { keep << files_[(idx_ + 1) % n] << files_[(idx_ + n - 1) % n]; }
    for (auto it = cache_.begin(); it != cache_.end();) { if (!keep.contains(it->first)) it = cache_.erase(it); else ++it; }
    if (n < 2 || !view_->image() || view_->image()->bytes() > (400u << 20)) return;
    const QString& p = files_[(idx_ + 1) % n];
    QFileInfo fi(p);
    if (is_3d_ext(lower_ext(p.toStdString())) || cache_.count(p) || jobs_.count(p) || fi.size() > (200 << 20)) return;
    startJob(p);
}

// ---------------------------------------------------------------- status / title
void MainWindow::updateTitle() {
    QString t;
    if (viewer_) t = QFileInfo(want_).fileName();
    else if (part_) t = part_->url().fileName().isEmpty() ? part_->url().toDisplayString() : part_->url().fileName();
    setWindowTitle(t.isEmpty() ? "HB Viewer" : t + " - HB Viewer");
}

void MainWindow::updateStatus() {
    QString l, r;
    if (!viewer_) {
        l = partStatus_;
        if (selection_.size() == 1) {
            const KFileItem& it = selection_.first();
            l = it.text() + (it.isDir() ? QString() : "   " + humanSize((double)it.size())) + "   " +
                it.time(KFileItem::ModificationTime).toString("yyyy-MM-dd HH:mm") + "   " + it.mimeComment();
        }
        statusL_->setText(l);
        statusR_->setText(part_ ? QString() : "Dolphin not available");
        return;
    }
    if (!files_.isEmpty() && idx_ >= 0) l = QString("%1/%2   %3").arg(idx_ + 1).arg(files_.size()).arg(QFileInfo(want_).fileName());
#ifdef HBV_HAVE_F3D
    if (showingModel_ && model_) {
        if (!model_->error().isEmpty()) l += "   cannot open: " + model_->error().left(200);
        else l += "   3D model   " + humanSize((double)QFileInfo(want_).size());
        statusL_->setText(l);
        statusR_->setText("f3d " + ModelView::f3dVersion() + "   drag: rotate, middle: pan, wheel: zoom");
        return;
    }
#endif
    auto res = view_->result();
    if (jobs_.count(want_) && (curPath_ != want_ || (res && res->info.preview))) {
        l += QString("   loading%1%2%").arg(curPath_ == want_ ? " full resolution " : " ").arg(jobs_[want_].ctl->progress / 10);
        if (curPath_ == want_ && res && res->info.orig_w)
            l += QString("   %1 x %2  %3").arg(res->info.orig_w).arg(res->info.orig_h).arg(QString::fromStdString(res->info.format));
    } else if (res && curPath_ == want_) {
        const ImageInfo& in = res->info;
        if (view_->image()) {
            l += QString("   %1 x %2").arg(in.orig_w).arg(in.orig_h);
            if (in.scale_div > 1) l += QString(" (shown at 1/%1)").arg(in.scale_div);
            l += "   " + QString::fromStdString(in.format);
            if (!in.details.empty()) l += " " + QString::fromStdString(in.details);
        }
        l += "   " + humanSize((double)QFileInfo(want_).size());
        if (loadSecs_ > 0) l += QString("   %1 s").arg(loadSecs_, 0, 'f', 2);
    }
    if (view_->image()) {
        r = view_->pixelText();
        r += QString("%1%2%").arg(r.isEmpty() ? "" : "   ").arg(view_->zoomPercent(), 0, 'f', 1);
        if (slideshow_->isActive()) r = "Slideshow   " + r;
    }
    statusL_->setText(l);
    statusR_->setText(r);
}

// ---------------------------------------------------------------- commands
QList<QUrl> MainWindow::targets() const {
    if (viewer_) return want_.isEmpty() ? QList<QUrl>() : QList<QUrl>{QUrl::fromLocalFile(want_)};
    return selection_.urlList();
}

QString MainWindow::targetPath() const {
    QList<QUrl> t = targets();
    if (t.isEmpty()) return {};
    return t.first().isLocalFile() ? t.first().toLocalFile() : t.first().toDisplayString();
}

void MainWindow::cmdOpen() {
    QString start = viewer_ && !want_.isEmpty() ? QFileInfo(want_).absolutePath()
                  : part_ && part_->url().isLocalFile() ? part_->url().toLocalFile() : QDir::homePath();
    QString f = QFileDialog::getOpenFileName(this, "Open", start);
    if (!f.isEmpty()) openPath(f);
}

void MainWindow::cmdSaveAs() {
    QImage q = view_->transformedImage();
    if (q.isNull()) { QMessageBox::warning(this, "Save as", view_->image() ? "Image is too large to save (over 2 GB of pixels)." : "No image."); return; }
    QString base = QFileInfo(want_).completeBaseName() + ".png";
    QString f = QFileDialog::getSaveFileName(this, "Save as", QFileInfo(want_).absoluteDir().filePath(base),
                                             "Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff *.avif *.jxl)");
    if (f.isEmpty()) return;
    std::string e = lower_ext(f.toStdString());
    if (e == "jpg" || e == "jpeg" || e == "bmp") {           // no alpha: flatten on white
        QImage o(q.size(), QImage::Format_RGB32);
        o.fill(Qt::white);
        QPainter p(&o); p.drawImage(0, 0, q); p.end();
        q = o;
    }
    QImageWriter w(f);
    w.setQuality(92);
    if (!w.write(q)) QMessageBox::critical(this, "Save as", w.errorString());
}

void MainWindow::cmdRename() {
    QList<QUrl> t = targets();
    if (t.isEmpty()) return;
    KFileItemList items;
    for (const QUrl& u : t) items.append(KFileItem(u));
    auto* dlg = new KIO::RenameFileDialog(items, this);
    connect(dlg, &KIO::RenameFileDialog::renamingFinished, this, [this, t](const QList<QUrl>& renamed) {
        if (!viewer_ || renamed.isEmpty() || !renamed.first().isLocalFile()) return;
        QString oldp = t.first().toLocalFile(), np = renamed.first().toLocalFile();
        int i = files_.indexOf(oldp);
        if (i >= 0) files_[i] = np;
        if (cache_.count(oldp)) { cache_[np] = cache_[oldp]; cache_.erase(oldp); }
        if (curPath_ == oldp) curPath_ = np;
        if (want_ == oldp) want_ = np;
        updateTitle(); updateStatus();
    });
    dlg->open();
}

void MainWindow::cmdTrash() {
    QList<QUrl> t = targets();
    if (t.isEmpty()) return;
    if (confirmDelete_) {
        QString q = t.size() == 1 ? QString("Move \"%1\" to the trash?").arg(t.first().fileName())
                                  : QString("Move %1 items to the trash?").arg(t.size());
        if (QMessageBox::question(this, "Move to trash", q) != QMessageBox::Yes) return;
    }
    KIO::Job* job = KIO::trash(t);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
    if (viewer_) {
        QString gone = want_;
        int i = files_.indexOf(gone);
        if (i >= 0) files_.removeAt(i);
        cache_.erase(gone);
        if (files_.isEmpty()) { view_->clear(); want_.clear(); curPath_.clear(); idx_ = -1; updateStatus(); return; }
        idx_ = std::min(std::max(i, 0), (int)files_.size() - 1);
        openInViewer(files_[idx_]);
    }
}

void MainWindow::cmdInfo() {
    if (!viewer_) {
        if (!selection_.isEmpty()) KPropertiesDialog::showDialog(selection_, this, false);
        return;
    }
    if (want_.isEmpty()) return;
    QFileInfo fi(want_);
    QString s = "File: " + want_ + "\nType: " + QMimeDatabase().mimeTypeForFile(want_).comment() +
                "\nSize: " + humanSize((double)fi.size()) + QString(" (%1 bytes)").arg(fi.size()) +
                "\nModified: " + fi.lastModified().toString("yyyy-MM-dd HH:mm:ss") + "\n";
    auto r = view_->result();
    if (r && curPath_ == want_) {
        const ImageInfo& in = r->info;
        if (Image* im = view_->image()) {
            s += "Format: " + QString::fromStdString(in.format) + "\n";
            if (!in.details.empty()) s += "Details: " + QString::fromStdString(in.details) + "\n";
            s += QString("Dimensions: %1 x %2 (%3 MP)\n").arg(in.orig_w).arg(in.orig_h).arg((double)in.orig_w * in.orig_h / 1e6, 0, 'f', 1);
            if (in.scale_div > 1) s += QString("Shown at 1/%1 (%2 x %3) to stay within the memory budget\n").arg(in.scale_div).arg(im->width()).arg(im->height());
            s += QString("Memory used: %1 (%2 mip levels)\n").arg(humanSize((double)im->bytes())).arg(im->lv.size());
            s += QString("Transparency: %1\n").arg(im->has_alpha ? "yes" : "no");
            if (loadSecs_ > 0) s += QString("Load time: %1 s\n").arg(loadSecs_, 0, 'f', 3);
        } else if (!r->error.empty()) s += "Error: " + QString::fromStdString(r->error) + "\n";
    }
    s += QString("\nDecode budget: %1 MP").arg(budget_ / 1e6, 0, 'f', 0);
    QMessageBox::information(this, "File info", s);
}

void MainWindow::cmdOpenWith() {
    QList<QUrl> t = targets();
    if (t.isEmpty()) return;
    auto* job = new KIO::ApplicationLauncherJob();          // no service -> "Open With" dialog
    job->setUrls(t);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
    job->start();
}

void MainWindow::cmdF3d() {
    QString t = targetPath();
    if (t.isEmpty()) return;
    QString f3d;
    if (QByteArray ad = qgetenv("APPDIR"); !ad.isEmpty() && QFileInfo(QString::fromLocal8Bit(ad) + "/usr/bin/f3d").isExecutable())
        f3d = QString::fromLocal8Bit(ad) + "/usr/bin/f3d";
    else f3d = QStandardPaths::findExecutable("f3d");
    if (f3d.isEmpty()) { QMessageBox::information(this, "f3d", "f3d was not found.\nInstall it (https://f3d.app) or put it in your PATH."); return; }
    QProcess::startDetached(f3d, {t});
}

void MainWindow::toggleFullscreen() {
    if (isFullScreen()) {
        if (wasMaximized_) showMaximized(); else showNormal();
        tb_->setVisible(ini_->value("window/toolbar", true).toBool());
        statusBar()->setVisible(ini_->value("window/statusbar", true).toBool());
    } else {
        saveSettings();
        wasMaximized_ = isMaximized();
        tb_->hide(); statusBar()->hide();
        showFullScreen();
    }
}

void MainWindow::shortcutsHelp() {
    QString s;
    for (const QString& g : kGroups) {
        QString block;
        for (const Command& c : cmds_) {
            if (c.group != g) continue;
            QStringList keys;
            for (const QKeySequence& k : c.action->shortcuts()) keys << k.toString(QKeySequence::NativeText);
            block += "&nbsp;&nbsp;" + c.label.toHtmlEscaped() + (keys.isEmpty() ? QString() : "&nbsp;&nbsp;<i>[" + keys.join(", ").toHtmlEscaped() + "]</i>") + "<br>";
        }
        if (!block.isEmpty()) s += "<b>" + g + "</b><br>" + block + "<br>";
    }
    s += "<b>Mouse</b><br>Viewer: drag = pan, wheel = zoom (Ctrl+wheel navigates; M swaps), double-click = full screen, "
         "middle-click = fit, back/forward buttons = previous/next.<br>Right-click a command in any menu to add it to or "
         "remove it from the Favorites toolbar.";
    QDialog d(this);
    d.setWindowTitle("Commands and shortcuts");
    auto* lay = new QVBoxLayout(&d);
    auto* sa = new QScrollArea;
    auto* lab = new QLabel(s);
    lab->setTextFormat(Qt::RichText);
    lab->setWordWrap(true);
    lab->setMargin(10);
    sa->setWidget(lab);
    sa->setWidgetResizable(true);
    lay->addWidget(sa);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    lay->addWidget(bb);
    d.resize(600, 640);
    d.exec();
}
