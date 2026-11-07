#include "previewpane.h"
#include "decode.h"
#include "imageview.h"
#ifdef HBV_HAVE_F3D
#include "modelview.h"
#endif
#include "thumb3d.h"
#include <KIO/PreviewJob>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QApplication>
#include <QStackedWidget>
#include <QWheelEvent>
#include <QToolButton>
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
    stack_ = new QStackedWidget;
    stack_->setMinimumHeight(200);
    stack_->addWidget(view_);
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

    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    info_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    l->addWidget(stack_, 1);
    l->addWidget(tools_);
    l->addWidget(info_);
}

PreviewPane::~PreviewPane() { cancelLoad(); }

void PreviewPane::cancelLoad() {
    if (ctl_) ctl_->cancel = true;
    ctl_.reset();
}

// keys work while the canvas has focus (click it first), so the browser keeps its own keys
bool PreviewPane::eventFilter(QObject* o, QEvent* e) {
    if (o == view_ && is3d_ && !live_ && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::Wheel)) {
        goLive3D(e);
        return true;
    }
#ifdef HBV_HAVE_F3D
    if (o == view_ && live_ && model_ && (e->type() == QEvent::MouseMove || e->type() == QEvent::MouseButtonRelease)) {
        auto* m = static_cast<QMouseEvent*>(e);       // drag that started on the still: keep it going
        QMouseEvent ev(e->type(), model_->mapFromGlobal(m->globalPosition()), m->scenePosition(), m->globalPosition(),
                       m->button(), m->buttons(), m->modifiers());
        QApplication::sendEvent(model_, &ev);
        return true;
    }
#endif
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

// The still render is cheap to browse through; the first click / wheel on it loads the model
// into a real f3d view (same controls as the 3D viewer) and hands that event over.
void PreviewPane::goLive3D(QEvent* forward) {
#ifdef HBV_HAVE_F3D
    if (!is3d_ || path3d_.isEmpty()) return;
    if (!model_) {
        model_ = new ModelView;
        stack_->addWidget(model_);
    }
    live_ = true;
    tools_->hide();
    stack_->setCurrentWidget(model_);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    model_->load(path3d_);
    QApplication::restoreOverrideCursor();
    model_->setFocus();
    info_->setText(baseInfo_ + (model_->error().isEmpty()
        ? QStringLiteral("<br>3D model - drag to orbit, right-drag/wheel to zoom, middle-drag to pan")
        : "<br>" + model_->error().left(200).toHtmlEscaped()));
    if (!forward) return;
    if (forward->type() == QEvent::MouseButtonPress) {
        auto* m = static_cast<QMouseEvent*>(forward);
        QMouseEvent ev(QEvent::MouseButtonPress, m->position(), m->scenePosition(), m->globalPosition(),
                       m->button(), m->buttons(), m->modifiers());
        QApplication::sendEvent(model_, &ev);
    } else if (forward->type() == QEvent::Wheel) {
        auto* w = static_cast<QWheelEvent*>(forward);
        QWheelEvent ev(w->position(), w->globalPosition(), w->pixelDelta(), w->angleDelta(), w->buttons(),
                       w->modifiers(), w->phase(), w->inverted());
        QApplication::sendEvent(model_, &ev);
    }
#else
    (void)forward;
#endif
}

void PreviewPane::leaveLive3D() {
#ifdef HBV_HAVE_F3D
    if (live_ && model_) model_->unload();          // frees the model's memory
#endif
    live_ = false;
    stack_->setCurrentWidget(view_);
    tools_->show();
}

void PreviewPane::clearItem() {
    ++gen_;
    cancelLoad();
    leaveLive3D();
    is3d_ = false;
    item_ = KFileItem();
    view_->clear();
    info_->setText("<i>Nothing selected</i>");
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
    leaveLive3D();
    is3d_ = false;
    item_ = it;
    setPicture(QIcon::fromTheme(it.iconName()).pixmap(128, 128).toImage());
    baseInfo_ = "<b>" + it.text().toHtmlEscaped() + "</b><br>" + it.mimeComment().toHtmlEscaped() +
                (it.isDir() ? QString() : "<br>" + humanSize((double)it.size())) +
                "<br>" + it.time(KFileItem::ModificationTime).toString("yyyy-MM-dd HH:mm");
    info_->setText(baseInfo_);
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
                        self->info_->setText(self->baseInfo_ + extra);
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
        is3d_ = true;
        path3d_ = it.localPath();
        info_->setText(baseInfo_ + "<br><i>rendering 3D preview...</i>");
        Thumb3D::request(it.localPath(), 1024, this, [this, gen](const QImage& img, const QString& err) {
            if (gen != gen_ || live_) return;
            setPicture(img);
            info_->setText(baseInfo_ + (err.isEmpty() ? QStringLiteral("<br>3D model - click or drag to orbit") : "<br>" + err.left(200).toHtmlEscaped()));
        });
        return;
    }
    auto* job = KIO::filePreview(KFileItemList{it}, QSize(1024, 1024));
    connect(job, &KIO::PreviewJob::gotPreview, this, [this, gen](const KFileItem&, const QPixmap& pm) {
        if (gen == gen_) setPicture(pm.toImage());
    });
}
