// Cascade (Miller column) explorer: each selected folder opens its contents in the next
// column; the last column previews the selected file. Built on KIO (KDirModel), so it shows
// the same files, icons, thumbnails and remote locations (smb:/, sftp:/...) as Dolphin.
#pragma once
#include <KFileItem>
#include <QColumnView>
#include <QPersistentModelIndex>
#include <QUrl>
#include <QWidget>

class KDirModel;
class KDirSortFilterProxyModel;
class KFilePreviewGenerator;
class PreviewPane;

class CascadeView : public QColumnView {          // gives every column our context menu
    Q_OBJECT
public:
    using QColumnView::QColumnView;
signals:
    void menuRequested(QAbstractItemView* column, QPoint pos);
    void navKey(int key);                           // arrow keys / Enter pressed in any column
protected:
    QAbstractItemView* createColumn(const QModelIndex& root) override;
    bool eventFilter(QObject* o, QEvent* e) override;
};

class ColumnBrowser : public QWidget {
    Q_OBJECT
public:
    explicit ColumnBrowser(QWidget* parent = nullptr);

    void setUrl(const QUrl& folder, const QUrl& select = QUrl());
    QUrl url() const { return folder_; }                 // folder of the current item
    KFileItemList selectedItems() const;
    void setShowHidden(bool on);
    bool showHidden() const { return hidden_; }
    QAbstractItemView* view() const { return view_; }
    int columnCount() const;                             // for the self-test
    QUrl currentUrl() const;                             // item with the keyboard cursor
    void pressKey(int key);                              // self-test: simulate a key press
    int currentRow() const;
    int siblingCount() const;                            // rows in the current item's column

signals:
    void activated(const KFileItem& item);              // double-click / Enter on a file
    void contextMenu(const QPoint& global, const KFileItemList& items);
    void selectionChanged(const KFileItemList& items);
    void folderChanged(const QUrl& folder);

private:
    QUrl rootFor(const QUrl& u) const;
    KFileItem itemAt(const QModelIndex& proxyIndex) const;
    void showPreview(const QModelIndex& proxyIndex);
    void goLeft();
    void goRight();
    void goVertical(int delta);
    void focusColumnOf(const QModelIndex& proxyIndex);
    void selectIndex(const QModelIndex& proxyIndex);     // current + selected + focus its column
    void selectFirstChildOfPending();

    CascadeView* view_;
    KDirModel* model_;
    KDirSortFilterProxyModel* proxy_;
    KFilePreviewGenerator* gen_;
    PreviewPane* pane_;
    QUrl root_, folder_, target_;
    bool hidden_ = false;
    QPersistentModelIndex enterPending_;              // Right pressed on a folder still being listed
};
