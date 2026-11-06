#include "columnbrowser.h"
#include "decode.h"
#include "previewpane.h"
#include <KDirLister>
#include <KDirModel>
#include <KDirSortFilterProxyModel>
#include <KFilePreviewGenerator>
#include <QDir>
#include <QIcon>
#include <QCoreApplication>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QPointer>
#include <QVBoxLayout>
#include <thread>

QAbstractItemView* CascadeView::createColumn(const QModelIndex& root) {
    QAbstractItemView* col = QColumnView::createColumn(root);
    col->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(col, &QWidget::customContextMenuRequested, this, [this, col](const QPoint& p) { emit menuRequested(col, p); });
    return col;
}


ColumnBrowser::ColumnBrowser(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    model_ = new KDirModel(this);
    model_->dirLister()->setAutoErrorHandlingEnabled(false);
    proxy_ = new KDirSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortFoldersFirst(true);
    proxy_->sort(KDirModel::Name, Qt::AscendingOrder);

    view_ = new CascadeView(this);
    view_->setModel(proxy_);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setDragEnabled(true);
    view_->setDragDropMode(QAbstractItemView::DragOnly);
    view_->setResizeGripsVisible(true);
    view_->setIconSize(QSize(22, 22));
    lay->addWidget(view_);

    gen_ = new KFilePreviewGenerator(view_);             // small KIO thumbnails in the columns
    gen_->setPreviewShown(true);

    // preview column for the selected file
    pane_ = new PreviewPane;
    pane_->setMinimumWidth(320);
    view_->setPreviewWidget(pane_);

    connect(view_, &QColumnView::updatePreviewWidget, this, &ColumnBrowser::showPreview);
    connect(view_, &QAbstractItemView::activated, this, [this](const QModelIndex& i) {
        KFileItem it = itemAt(i);
        if (!it.isNull() && !it.isDir()) emit activated(it);   // folders just open the next column
    });
    connect(view_, &CascadeView::menuRequested, this, [this](QAbstractItemView* col, const QPoint& p) {
        QModelIndex i = col->indexAt(p);
        if (i.isValid() && !view_->selectionModel()->isSelected(i)) view_->setCurrentIndex(i);
        KFileItemList items = selectedItems();
        if (!i.isValid()) {                              // empty space: the column's folder
            KFileItem dir = model_->itemForIndex(proxy_->mapToSource(col->rootIndex()));
            items = dir.isNull() ? KFileItemList{KFileItem(root_)} : KFileItemList{dir};
        }
        emit contextMenu(col->viewport()->mapToGlobal(p), items);
    });
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { emit selectionChanged(selectedItems()); });
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& cur) {
        KFileItem it = itemAt(cur);
        if (it.isNull()) return;
        QUrl f = it.isDir() ? it.url() : it.url().adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash);
        if (f != folder_) { folder_ = f; emit folderChanged(f); }
    });
    // after setUrl(): select the target once KIO has listed the way down to it
    connect(model_, &KDirModel::expand, this, [this](const QModelIndex& src) {
        KFileItem it = model_->itemForIndex(src);
        if (it.isNull() || !target_.isValid()) return;
        if (it.url().adjusted(QUrl::StripTrailingSlash) == target_.adjusted(QUrl::StripTrailingSlash)) {
            QModelIndex p = proxy_->mapFromSource(src);
            view_->setCurrentIndex(p);
            view_->selectionModel()->select(p, QItemSelectionModel::ClearAndSelect);
            view_->scrollTo(p);
            target_.clear();
        }
    });
}

KFileItem ColumnBrowser::itemAt(const QModelIndex& i) const {
    return i.isValid() ? model_->itemForIndex(proxy_->mapToSource(i)) : KFileItem();
}

KFileItemList ColumnBrowser::selectedItems() const {
    KFileItemList l;
    for (const QModelIndex& i : view_->selectionModel()->selectedIndexes())
        if (i.column() == 0) { KFileItem it = itemAt(i); if (!it.isNull()) l.append(it); }
    return l;
}

int ColumnBrowser::columnCount() const { return view_->findChildren<QAbstractItemView*>().size(); }

QUrl ColumnBrowser::rootFor(const QUrl& u) const {
    if (u.isLocalFile()) {
        QString home = QDir::homePath(), p = u.toLocalFile();
        return QUrl::fromLocalFile(p == home || p.startsWith(home + "/") ? home : QStringLiteral("/"));
    }
    QUrl r = u;
    r.setPath(QStringLiteral("/"));
    return r;
}

void ColumnBrowser::setUrl(const QUrl& folder, const QUrl& select) {
    if (!folder.isValid()) return;
    QUrl root = rootFor(folder);
    folder_ = folder.adjusted(QUrl::StripTrailingSlash);
    target_ = select.isValid() ? select : folder;
    if (root != root_) {
        root_ = root;
        model_->openUrl(root);                           // columns start at Home or /
        view_->setRootIndex(QModelIndex());
    }
    if (target_.adjusted(QUrl::StripTrailingSlash) == root_.adjusted(QUrl::StripTrailingSlash)) {
        target_.clear();
        view_->setCurrentIndex(QModelIndex());
        return;
    }
    model_->expandToUrl(target_);
}

void ColumnBrowser::setShowHidden(bool on) {
    hidden_ = on;
    model_->dirLister()->setShowHiddenFiles(on);
    model_->dirLister()->emitChanges();
}

void ColumnBrowser::showPreview(const QModelIndex& i) { pane_->showItem(itemAt(i)); }
