#include "previewpane.h"
#include "decode.h"
#include "imageview.h"
#include "thumb3d.h"
#include <KIO/PreviewJob>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <thread>

static QString humanSize(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"}; int k = 0;
    while (b >= 1024 && k < 4) { b /= 1024; k++; }
    return k ? QString::number(b, 'f', 1) + " " + u[k] : QString::number((qint64)b) + " B";
}

static constexpr uint64_t kFullBudget = 64000000;   // full-res decode limit for the pane (px)

PreviewPane::PreviewPane(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(220);
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(2, 2, 2, 2);
    view_ = new ImageView;
    view_->setMinimumHeight(200);
    view_->wheelZoom = true;
    view_->fitOnOpen = true;
    view_->installEventFilter(this);
    connect(view_, &ImageView::doubleClicked, this, [this] {   // toggle fit <-> actual pixels
        if (view_->zoomPercent() > 99.5 && view_->zoomPercent() < 100.5) view_->fit(true); else view_->actualPixels();
    });

    tools_ = new QWidget;
    auto* tl = new QHBoxLayout(tools_);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(1);
    auto btn = [&](const char* icon, const QString& tip, auto fn) {
        auto* b = new QToolButton;
        b->setIcon(QIcon::fromTheme(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QToolButton::clicked, this, fn);
        tl->addWidget(b);
    };
    btn("zoom-out", tr("Zoom out (-)"), [this] { view_->zoomStep(-1); });
    btn("zoom-in", tr("Zoom in (+)"), [this] { view_->zoomStep(1); });
    btn("zoom-fit-best", tr("Fit (0, middle click)"), [this] { view_->fit(true); });
    btn("zoom-original", tr("Actual pixels (1)"), [this] { view_->actualPixels(); });
    btn("zoom-fit-width", tr("Fit width (W)"), [this] { view_->fitWidth(); });
    btn("object-rotate-left", tr("Rotate left (L)"), [this] { view_->rotate(-1); });
    btn("object-rotate-right", tr("Rotate right (R)"), [this] { view_->rotate(1); });
    btn("object-flip-horizontal", tr("Flip horizontally (H)"), [this] { view_->flipH(); });
    tl->addStretch(1);
    auto* zoomLbl = new QLabel;
    zoomLbl->setMinimumWidth(48);
    zoomLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tl->addWidget(zoomLbl);
    connect(view_, &ImageView::statusChanged, zoomLbl, [this, zoomLbl] {
        zoomLbl->setText(view_->image() ? QString::number(view_->zoomPercent(), 'f', view_->zoomPercent() < 10 ? 1 : 0) + "%" : QString());
    });

    // file info: a single line under the preview; too long -> middle-drag or wheel scrolls it (Blender style)
    info_ = new QLabel;
    info_->setWordWrap(false);
    info_->setTextFormat(Qt::RichText);
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_->setAlignment(Qt::AlignVCenter | Qt::AlignHCenter);
    infoScroll_ = new QScrollArea;
    infoScroll_->setWidget(info_);
    infoScroll_->setWidgetResizable(false);
    infoScroll_->setFrameShape(QFrame::NoFrame);
    infoScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    infoScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    infoScroll_->setAlignment(Qt::AlignCenter);
    infoScroll_->setFixedHeight(info_->fontMetrics().height() + 8);
    infoScroll_->setToolTip(tr("Middle-drag or scroll to see the whole line"));
    info_->installEventFilter(this);
    infoScroll_->viewport()->installEventFilter(this);
    l->addWidget(view_, 1);
    l->addWidget(tools_);
    l->addWidget(infoScroll_);
}

PreviewPane::~PreviewPane() { cancelLoad(); }

void PreviewPane::cancelLoad() {
    if (ctl_) ctl_->cancel = true;
    ctl_.reset();
}

// keys work while the canvas has focus (click it first), so the browser keeps its own keys
bool PreviewPane::eventFilter(QObject* o, QEvent* e) {
    if (o == info_ || o == infoScroll_->viewport()) {
        QScrollBar* sb = infoScroll_->horizontalScrollBar();
        switch (e->type()) {
        case QEvent::MouseButtonPress: {
            auto* m = static_cast<QMouseEvent*>(e);
            if (m->button() != Qt::MiddleButton) break;
            dragX_ = (int)m->globalPosition().x(); dragStart_ = sb->value();
            infoScroll_->viewport()->setCursor(Qt::ClosedHandCursor);
            return true;
        }
        case QEvent::MouseMove:
            if (dragX_ < 0) break;
            sb->setValue(dragStart_ - ((int)static_cast<QMouseEvent*>(e)->globalPosition().x() - dragX_));
            return true;
        case QEvent::MouseButtonRelease:
            if (dragX_ < 0 || static_cast<QMouseEvent*>(e)->button() != Qt::MiddleButton) break;
            dragX_ = -1;
            infoScroll_->viewport()->unsetCursor();
            return true;
        case QEvent::Wheel: {
            auto* w = static_cast<QWheelEvent*>(e);
            int d = w->angleDelta().y() ? w->angleDelta().y() : w->angleDelta().x();
            sb->setValue(sb->value() - d / 2);
            return true;
        }
        default: break;
        }
        return false;
    }
    if (o == view_ && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
        switch (k->key()) {
        case Qt::Key_Plus: case Qt::Key_Equal: view_->zoomStep(1); return true;
        case Qt::Key_Minus: view_->zoomStep(-1); return true;
        case Qt::Key_0: case Qt::Key_Slash: view_->fit(true); return true;
        case Qt::Key_1: case Qt::Key_Asterisk: view_->actualPixels(); return true;
        case Qt::Key_W: view_->fitWidth(); return true;
        case Qt::Key_L: view_->rotate(-1); return true;
        case Qt::Key_R: view_->rotate(1); return true;
        case Qt::Key_H: view_->flipH(); return true;
        case Qt::Key_V: view_->flipV(); return true;
        default: break;
        }
    }
    return QWidget::eventFilter(o, e);
}

void PreviewPane::setInfo(const QString& html) {
    QString t = html;
    t.replace("<br>", QStringLiteral("&nbsp;&nbsp;\u00b7&nbsp;&nbsp;"));
    info_->setText(t);
    info_->adjustSize();
    infoScroll_->horizontalScrollBar()->setValue(0);
}

void PreviewPane::clearItem() {
    ++gen_;
    cancelLoad();
    item_ = KFileItem();
    view_->clear();
    setInfo("<i>Nothing selected</i>");
}

void PreviewPane::setPicture(const QImage& img) {
    if (img.isNull()) return;
    auto r = std::make_shared<LoadResult>();
    r->img = image_from_qimage(img);
    if (!r->img) return;
    r->info.orig_w = img.width(); r->info.orig_h = img.height();
    view_->setResult(r, QString(), false);
}

void PreviewPane::showItem(const KFileItem& it) {
    if (it.isNull()) { clearItem(); return; }
    int gen = ++gen_;
    cancelLoad();
    item_ = it;
    setPicture(QIcon::fromTheme(it.iconName()).pixmap(128, 128).toImage());
    baseInfo_ = "<b>" + it.text().toHtmlEscaped() + "</b><br>" + it.mimeComment().toHtmlEscaped() +
                (it.isDir() ? QString() : "<br>" + humanSize((double)it.size())) +
                "<br>" + it.time(KFileItem::ModificationTime).toString("yyyy-MM-dd HH:mm");
    setInfo(baseInfo_);
    if (it.isDir()) return;
    QPointer<PreviewPane> self(this);
    std::string ext = lower_ext(it.name().toStdString());
    if (it.isLocalFile() && is_image_ext(ext)) {
        // quick preview first, then the full-resolution decode (cancelled when the selection moves on)
        std::string path = it.localPath().toStdString();
        QString qpath = it.localPath();
        auto ctl = std::make_shared<LoadCtl>();
        ctl_ = ctl;
        std::thread([self, gen, path, qpath, ctl] {
            auto show = [self, gen, qpath](std::shared_ptr<LoadResult> r, bool upgrade) {
                QMetaObject::invokeMethod(qApp, [self, gen, qpath, r, upgrade] {
                    if (!self || gen != self->gen_ || !r->img) return;
                    if (!upgrade) {
                        QString extra = QString("<br>%1 x %2").arg(r->info.orig_w).arg(r->info.orig_h);
                        if (!r->info.format.empty()) extra += "  " + QString::fromStdString(r->info.format);
                        if (!r->info.details.empty()) extra += "<br>" + QString::fromStdString(r->info.details).toHtmlEscaped();
                        self->setInfo(self->baseInfo_ + extra);
                    }
                    self->view_->setResult(r, r->animated ? qpath : QString(), upgrade);
                }, Qt::QueuedConnection);
            };
            std::shared_ptr<LoadResult> p(load_preview(path, 1500000).release());
            if (ctl->cancel) return;
            if (p->img) p->info.preview = true;
            show(p, false);
            if (!p->img || p->animated || ctl->cancel) return;
            // already complete? (small images: the preview is the full image)
            if (p->info.scale_div == 1 && p->img->width() == p->info.orig_w && p->img->height() == p->info.orig_h) return;
            std::shared_ptr<LoadResult> f(load_image(path, kFullBudget, *ctl).release());
            if (ctl->cancel || !f->img) return;
            show(f, true);
        }).detach();
        return;
    }
    if (it.isLocalFile() && is_3d_ext(ext) && Thumb3D::available()) {
        // 3D model: rendered by our own offscreen f3d engine on a worker thread
        setInfo(baseInfo_ + "<br><i>rendering 3D preview...</i>");
        Thumb3D::request(it.localPath(), 1024, this, [this, gen](const QImage& img, const QString& err) {
            if (gen != gen_) return;
            setPicture(img);
            setInfo(baseInfo_ + (err.isEmpty() ? QStringLiteral("<br>3D model") : "<br>" + err.left(200).toHtmlEscaped()));
        });
        return;
    }
    auto* job = KIO::filePreview(KFileItemList{it}, QSize(1024, 1024));
    connect(job, &KIO::PreviewJob::gotPreview, this, [this, gen](const KFileItem&, const QPixmap& pm) {
        if (gen == gen_) setPicture(pm.toImage());
    });
}
