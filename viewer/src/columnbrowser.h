// Cascade (Miller column) explorer: each selected folder opens its contents in the next
// column; the last column previews the selected file. Built on KIO (KDirModel), so it shows
// the same files, icons, thumbnails and remote locations (smb:/, sftp:/...) as Dolphin.
#pragma once
#include <KFileItem>
#include <QColumnView>
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
protected:
    QAbstractItemView* createColumn(const QModelIndex& root) override;
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

signals:
    void activated(const KFileItem& item);              // double-click / Enter on a file
    void contextMenu(const QPoint& global, const KFileItemList& items);
    void selectionChanged(const KFileItemList& items);
    void folderChanged(const QUrl& folder);

private:
    QUrl rootFor(const QUrl& u) const;
    KFileItem itemAt(const QModelIndex& proxyIndex) const;
    void showPreview(const QModelIndex& proxyIndex);

    CascadeView* view_;
    KDirModel* model_;
    KDirSortFilterProxyModel* proxy_;
    KFilePreviewGenerator* gen_;
    PreviewPane* pane_;
    QUrl root_, folder_, target_;
    bool hidden_ = false;
};
