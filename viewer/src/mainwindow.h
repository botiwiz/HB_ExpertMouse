// Main window: Dolphin's browser component (DolphinPart) + our image viewer, sharing one
// command registry that feeds the right-click menus, shortcuts and the Favorites toolbar.
#pragma once
#include "decode.h"
#include <KFileItem>
#include <KParts/NavigationExtension>
#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <chrono>
#include <map>
#include <memory>

class ImageView;
class KUrlNavigator;
class KFilePlacesView;
class QLabel;
class QSettings;
class QStackedWidget;
class QToolBar;
class QTimer;
class QMenu;
namespace KParts { class ReadOnlyPart; class OpenUrlArguments; }

struct Command {
    QString id, label, group;
    QAction* action;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    void openPath(const QString& path);            // file -> viewer, folder -> browser
    void showBrowser(const QUrl& folder, const QUrl& select = QUrl());
    bool dolphinLoaded() const { return part_ != nullptr; }
    int commandCount() const { return cmds_.size(); }
    QString dolphinError() const { return partError_; }
    QString lastFolder() const;                    // browser folder from the previous session

protected:
    void closeEvent(QCloseEvent*) override;

private:
    // setup
    void loadDolphin();
    void buildCommands();
    void addDolphinCommands();
    QAction* addCmd(const QString& id, const QString& label, const QString& group, const QString& icon,
                    const QList<QKeySequence>& keys, std::function<void()> fn, QWidget* scope,
                    std::function<bool()> checked = nullptr);
    void loadSettings();
    void saveSettings();

    // menus & favorites
    QMenu* buildCommandMenu(QWidget* parent, QMenu* into = nullptr);
    void showCommandMenu(QPoint global);
    void browserPopup(const QPoint& global, const KFileItemList& items,
                      KParts::NavigationExtension::PopupFlags flags,
                      const KParts::NavigationExtension::ActionGroupMap& groups);
    void toggleFavorite(const QString& id);
    bool isFavorite(const QString& id) const { return favs_.contains(id); }
    void rebuildToolbar();
    void customizeFavorites();
    QString commandIdFor(QAction* a) const;
    QString menuText(const Command& c) const;

    // browser
    void onOpenUrlRequest(const QUrl& url, const KParts::OpenUrlArguments& args);
    void navigateTo(const QUrl& url);
    void triggerPartAction(const char* name);

    // viewer
    void setViewerMode(bool viewer);
    void viewFile(const QString& path);
    void scanFolder(const QString& file);
    void openInViewer(const QString& path);
    void go(int delta, bool absolute = false);
    void startJob(const QString& path);
    void showResult(const QString& path, std::shared_ptr<LoadResult> r);
    void preloadNeighbours();
    void updateStatus();
    void updateTitle();

    // commands
    QList<QUrl> targets() const;                   // selection in browser, current file in viewer
    QString targetPath() const;
    void cmdOpen();
    void cmdSaveAs();
    void cmdRename();
    void cmdTrash();
    void cmdInfo();
    void cmdOpenWith();
    void cmdF3d();
    void toggleFullscreen();
    void shortcutsHelp();

    // widgets
    QStackedWidget* stack_ = nullptr;
    QWidget* browserPage_ = nullptr;
    ImageView* view_ = nullptr;
    QToolBar* tb_ = nullptr;
    QLabel *statusL_ = nullptr, *statusR_ = nullptr;
    KUrlNavigator* nav_ = nullptr;
    KFilePlacesView* places_ = nullptr;
    QPointer<KParts::ReadOnlyPart> part_;
    QPointer<KParts::NavigationExtension> ext_;
    QString partError_, partStatus_;
    KFileItemList selection_;

    // commands
    QList<Command> cmds_;
    QStringList favs_;
    QSettings* ini_ = nullptr;

    // viewer state
    bool viewer_ = false, loopDir_ = true, confirmDelete_ = true, wasMaximized_ = false;
    QStringList files_;
    int idx_ = -1;
    QString want_, curPath_;
    double loadSecs_ = 0;
    uint64_t budget_ = 0;
    std::map<QString, std::shared_ptr<LoadResult>> cache_;
    struct Job { std::shared_ptr<LoadCtl> ctl; std::chrono::steady_clock::time_point start; };
    std::map<QString, Job> jobs_;
    QTimer *slideshow_ = nullptr, *progress_ = nullptr;
    int slideshowMs_ = 3000;
};
