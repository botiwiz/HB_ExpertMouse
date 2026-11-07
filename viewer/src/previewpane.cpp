#include "previewpane.h"
#include "decode.h"
#include "thumb3d.h"
#include <KIO/PreviewJob>
#include <QCoreApplication>
#include <QIcon>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <thread>

static QString humanSize(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"}; int k = 0;
    while (b >= 1024 && k < 4) { b /= 1024; k++; }
    return k ? QString::number(b, 'f', 1) + " " + u[k] : QString::number((qint64)b) + " B";
}

PreviewPane::PreviewPane(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(220);
    auto* l = new QVBoxLayout(this);
    image_ = new QLabel;
    image_->setAlignment(Qt::AlignCenter);
    image_->setMinimumHeight(200);
    image_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    l->addWidget(image_);
    l->addWidget(info_, 1);
}

void PreviewPane::clearItem() {
    ++gen_;
    item_ = KFileItem();
    full_ = QPixmap();
    image_->clear();
    info_->setText("<i>Nothing selected</i>");
}

void PreviewPane::setPicture(const QPixmap& pm) { full_ = pm; refit(); }

void PreviewPane::refit() {
    if (full_.isNull()) return;
    int w = std::max(64, image_->width() - 8), h = std::max(64, std::min(image_->width(), height() * 2 / 3));
    QPixmap p = full_;
    if (p.width() > w || p.height() > h) p = full_.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image_->setPixmap(p);
}

void PreviewPane::resizeEvent(QResizeEvent*) { refit(); }

void PreviewPane::showItem(const KFileItem& it) {
    if (it.isNull()) { clearItem(); return; }
    int gen = ++gen_;
    item_ = it;
    setPicture(QIcon::fromTheme(it.iconName()).pixmap(128, 128));
    baseInfo_ = "<b>" + it.text().toHtmlEscaped() + "</b><br>" + it.mimeComment().toHtmlEscaped() +
                (it.isDir() ? QString() : "<br>" + humanSize((double)it.size())) +
                "<br>" + it.time(KFileItem::ModificationTime).toString("yyyy-MM-dd HH:mm");
    info_->setText(baseInfo_);
    if (it.isDir()) return;
    QPointer<PreviewPane> self(this);
    std::string ext = lower_ext(it.name().toStdString());
    if (it.isLocalFile() && is_image_ext(ext)) {
        std::string path = it.localPath().toStdString();
        std::thread([self, gen, path] {
            std::shared_ptr<LoadResult> r(load_preview(path, 1500000).release());
            QMetaObject::invokeMethod(qApp, [self, gen, r] {
                if (!self || gen != self->gen_ || !r->img) return;
                self->setPicture(QPixmap::fromImage(qimage_from_image(r->img.get())));
                QString extra = QString("<br>%1 x %2").arg(r->info.orig_w).arg(r->info.orig_h);
                if (!r->info.format.empty()) extra += "  " + QString::fromStdString(r->info.format);
                if (!r->info.details.empty()) extra += "<br>" + QString::fromStdString(r->info.details).toHtmlEscaped();
                self->info_->setText(self->baseInfo_ + extra);
            }, Qt::QueuedConnection);
        }).detach();
        return;
    }
    if (it.isLocalFile() && is_3d_ext(ext) && Thumb3D::available()) {
        // 3D model: rendered by our own offscreen f3d engine on a worker thread
        info_->setText(baseInfo_ + "<br><i>rendering 3D preview...</i>");
        Thumb3D::request(it.localPath(), 512, this, [this, gen](const QImage& img, const QString& err) {
            if (gen != gen_) return;
            if (!img.isNull()) setPicture(QPixmap::fromImage(img));
            info_->setText(baseInfo_ + (err.isEmpty() ? QStringLiteral("<br>3D model") : "<br>" + err.left(200).toHtmlEscaped()));
        });
        return;
    }
    auto* job = KIO::filePreview(KFileItemList{it}, QSize(512, 512));
    connect(job, &KIO::PreviewJob::gotPreview, this, [this, gen](const KFileItem&, const QPixmap& pm) {
        if (gen == gen_) setPicture(pm);
    });
}
