#include "columnbrowser.h"
#include "decode.h"
#include <KDirLister>
#include <KDirModel>
#include <KDirSortFilterProxyModel>
#include <KFilePreviewGenerator>
#include <KIO/PreviewJob>
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

static QString humanSize(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"}; int k = 0;
    while (b >= 1024 && k < 4) { b /= 1024; k++; }
    return k ? QString::number(b, 'f', 1) + " " + u[k] : QString::number((qint64)b) + " B";
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
    pane_ = new QWidget;
    pane_->setMinimumWidth(320);
    auto* pl = new QVBoxLayout(pane_);
    paneImage_ = new QLabel;
    paneImage_->setAlignment(Qt::AlignCenter);
    paneImage_->setMinimumSize(300, 300);
    paneInfo_ = new QLabel;
    paneInfo_->setWordWrap(true);
    paneInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    paneInfo_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    pl->addWidget(paneImage_);
    pl->addWidget(paneInfo_, 1);
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

void ColumnBrowser::showPreview(const QModelIndex& i) {
    KFileItem it = itemAt(i);
    int gen = ++previewGen_;
    paneImage_->setPixmap(QIcon::fromTheme(it.iconName()).pixmap(128, 128));
    QString info = "<b>" + it.text().toHtmlEscaped() + "</b><br>" + it.mimeComment().toHtmlEscaped() +
                   "<br>" + humanSize((double)it.size()) +
                   "<br>" + it.time(KFileItem::ModificationTime).toString("yyyy-MM-dd HH:mm");
    paneInfo_->setText(info);
    if (it.isNull() || it.isDir()) return;
    QPointer<ColumnBrowser> self(this);
    std::string ext = lower_ext(it.name().toStdString());
    if (it.isLocalFile() && is_image_ext(ext)) {
        // our decoder: instant even for multi-GB PSD/PSB
        std::string path = it.localPath().toStdString();
        std::thread([self, gen, path, info] {
            std::shared_ptr<LoadResult> r(load_preview(path, 600000).release());
            QMetaObject::invokeMethod(qApp, [self, gen, r, info] {
                if (!self || gen != self->previewGen_ || !r->img) return;
                QImage q = qimage_from_image(r->img.get());
                self->paneImage_->setPixmap(QPixmap::fromImage(q.scaled(300, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                self->paneInfo_->setText(info + QString("<br>%1 x %2").arg(r->info.orig_w).arg(r->info.orig_h) +
                                         (r->info.format.empty() ? QString() : "  " + QString::fromStdString(r->info.format)));
            }, Qt::QueuedConnection);
        }).detach();
        return;
    }
    // everything else: the system's KIO thumbnailers (videos, PDFs, documents, fonts...)
    auto* job = KIO::filePreview(KFileItemList{it}, QSize(300, 300));
    job->setIgnoreMaximumSize(false);
    connect(job, &KIO::PreviewJob::gotPreview, this, [this, gen](const KFileItem&, const QPixmap& pm) {
        if (gen == previewGen_) paneImage_->setPixmap(pm);
    });
}
