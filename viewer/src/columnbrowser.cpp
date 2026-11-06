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
#include <QKeyEvent>
#include <QTimer>
#include <algorithm>
#include <iterator>
#include <QLabel>
#include <QListView>
#include <QPointer>
#include <QVBoxLayout>
#include <thread>

QAbstractItemView* CascadeView::createColumn(const QModelIndex& root) {
    QAbstractItemView* col = QColumnView::createColumn(root);
    col->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(col, &QWidget::customContextMenuRequested, this, [this, col](const QPoint& p) { emit menuRequested(col, p); });
    col->installEventFilter(this);
    return col;
}

// Left/Right are handled by ColumnBrowser (Qt's own handling needs already-listed folders)
bool CascadeView::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        // the cascade's own "current item" drives navigation, whichever column has focus
        static const int keys[] = {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down, Qt::Key_Return, Qt::Key_Enter};
        if (std::find(std::begin(keys), std::end(keys), k->key()) != std::end(keys) && !(k->modifiers() & ~Qt::KeypadModifier)) {
            emit navKey(k->key());
            return true;
        }
    }
    return QColumnView::eventFilter(o, e);
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
    connect(view_, &CascadeView::navKey, this, &ColumnBrowser::pressKey);
    // Right on a folder that KIO is still listing: pick its first item as soon as items arrive,
    // and once more when the listing completes (sorting may have changed which item is first)
    connect(proxy_, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex& parent) {
        if (enterPending_.isValid() && parent == QModelIndex(enterPending_)) selectFirstChildOfPending();
    });
    connect(model_->dirLister(), &KCoreDirLister::listingDirCompleted, this, [this](const QUrl& u) {
        if (!enterPending_.isValid()) return;
        KFileItem it = itemAt(enterPending_);
        if (!it.isNull() && it.url().adjusted(QUrl::StripTrailingSlash) == u.adjusted(QUrl::StripTrailingSlash)) {
            selectFirstChildOfPending();
            enterPending_ = QPersistentModelIndex();
        }
    });
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

void ColumnBrowser::selectIndex(const QModelIndex& i) {
    if (!i.isValid()) return;
    view_->setCurrentIndex(i);
    view_->selectionModel()->select(i, QItemSelectionModel::ClearAndSelect);
    // keyboard focus goes to the column showing this item; QColumnView may create that column
    // only after this call, so try now and again once the event loop has run
    QPersistentModelIndex p(i);
    focusColumnOf(i);
    QTimer::singleShot(0, this, [this, p] { if (p.isValid() && view_->currentIndex() == QModelIndex(p)) focusColumnOf(p); });
    QTimer::singleShot(60, this, [this, p] { if (p.isValid() && view_->currentIndex() == QModelIndex(p)) focusColumnOf(p); });
}

void ColumnBrowser::selectFirstChildOfPending() {
    QModelIndex parent = enterPending_;
    if (!parent.isValid() || proxy_->rowCount(parent) == 0) return;
    QModelIndex cur = view_->currentIndex();
    // only while the user hasn't moved somewhere else in the meantime
    if (cur != parent && cur.parent() != parent) { enterPending_ = QPersistentModelIndex(); return; }
    selectIndex(proxy_->index(0, 0, parent));
}

QUrl ColumnBrowser::currentUrl() const { KFileItem it = itemAt(view_->currentIndex()); return it.isNull() ? QUrl() : it.url(); }
void ColumnBrowser::pressKey(int key) {
    switch (key) {
    case Qt::Key_Right: goRight(); break;
    case Qt::Key_Left: goLeft(); break;
    case Qt::Key_Up: goVertical(-1); break;
    case Qt::Key_Down: goVertical(1); break;
    case Qt::Key_Return: case Qt::Key_Enter: {
        KFileItem it = itemAt(view_->currentIndex());
        if (it.isNull()) break;
        if (it.isDir()) goRight(); else emit activated(it);
        break;
    }
    }
}

int ColumnBrowser::currentRow() const { return view_->currentIndex().row(); }

void ColumnBrowser::goVertical(int delta) {
    enterPending_ = QPersistentModelIndex();
    QModelIndex cur = view_->currentIndex();
    if (!cur.isValid()) {
        if (proxy_->rowCount(view_->rootIndex()) > 0) selectIndex(proxy_->index(0, 0, view_->rootIndex()));
        return;
    }
    QModelIndex next = cur.sibling(cur.row() + delta, 0);
    if (next.isValid()) selectIndex(next);
}

void ColumnBrowser::focusColumnOf(const QModelIndex& i) {
    for (QAbstractItemView* col : view_->findChildren<QAbstractItemView*>())
        if (col != view_ && col->rootIndex() == i.parent()) {
            if (col->currentIndex() != i) col->setCurrentIndex(i);   // keeps the column's own cursor in step
            col->setFocus();
            col->scrollTo(i);
            return;
        }
}

void ColumnBrowser::goLeft() {
    enterPending_ = QPersistentModelIndex();
    QModelIndex cur = view_->currentIndex();
    if (!cur.isValid()) return;
    QModelIndex parent = cur.parent();
    if (parent.isValid()) selectIndex(parent);           // the parent folder, its contents stay open on the right
}

void ColumnBrowser::goRight() {
    QModelIndex cur = view_->currentIndex();
    if (!cur.isValid()) {                                  // nothing selected yet: first item of the root column
        if (proxy_->rowCount(view_->rootIndex()) > 0) selectIndex(proxy_->index(0, 0, view_->rootIndex()));
        return;
    }
    KFileItem it = itemAt(cur);
    if (it.isNull() || !it.isDir()) return;
    if (proxy_->rowCount(cur) > 0) { enterPending_ = QPersistentModelIndex(); selectIndex(proxy_->index(0, 0, cur)); return; }
    enterPending_ = QPersistentModelIndex(cur);            // not listed yet: list it, select when items arrive
    if (proxy_->canFetchMore(cur)) proxy_->fetchMore(cur);
}

void ColumnBrowser::showPreview(const QModelIndex& i) { pane_->showItem(itemAt(i)); }
