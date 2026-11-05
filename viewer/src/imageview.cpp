#include "imageview.h"
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

static const QBrush& checkerBrush() {
    static QBrush b = [] {
        QImage c(16, 16, QImage::Format_RGB32);
        c.fill(QColor(204, 204, 204));
        QPainter p(&c);
        p.fillRect(0, 0, 8, 8, QColor(153, 153, 153));
        p.fillRect(8, 8, 8, 8, QColor(153, 153, 153));
        return QBrush(c);
    }();
    return b;
}

ImageView::ImageView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

ImageView::~ImageView() = default;

void ImageView::clear() {
    if (movie_) { movie_->deleteLater(); movie_ = nullptr; }
    r_.reset();
    path_.clear();
    pixel_.clear();
    update();
}

void ImageView::setMessage(const QString& msg) { message_ = msg; update(); }

void ImageView::setResult(std::shared_ptr<LoadResult> r, const QString& path, bool keepView) {
    double oldW = keepView && image() ? image()->width() : 0;
    if (movie_) { movie_->deleteLater(); movie_ = nullptr; }
    r_ = std::move(r);
    path_ = path;
    message_.clear();
    if (keepView && oldW > 0 && image()) {
        if (autoFit_) fit(false);
        else zoom_ *= oldW / image()->width();
    } else {
        rot_ = 0; flipH_ = flipV_ = false;
        if (image()) {
            if (fitOnOpen) fit(false);
            else { zoom_ = 1; offx_ = offy_ = 0; autoFit_ = false; }
        }
    }
    if (r_ && r_->animated) startMovie();
    update();
    emit statusChanged();
}

void ImageView::startMovie() {
    movie_ = new QMovie(path_, QByteArray(), this);
    if (!movie_->isValid() || movie_->frameCount() == 1) { delete movie_; movie_ = nullptr; return; }
    connect(movie_, &QMovie::frameChanged, this, [this](int) {
        if (!r_ || !movie_) return;
        if (auto im = image_from_qimage(movie_->currentImage())) { im->build_mips(); r_->img = std::move(im); }
        update();
    });
    movie_->start();
}

void ImageView::rotatedSize(double& w, double& h) const {
    Image* im = image();
    w = im ? im->width() : 1; h = im ? im->height() : 1;
    if (rot_ & 1) std::swap(w, h);
}

QTransform ImageView::viewTransform() const {
    QTransform t;
    t.translate(width() / 2.0 + offx_, height() / 2.0 + offy_);
    t.rotate(rot_ * 90);
    t.scale(zoom_ * (flipH_ ? -1 : 1), zoom_ * (flipV_ ? -1 : 1));
    if (Image* im = image()) t.translate(-im->width() / 2.0, -im->height() / 2.0);
    return t;
}

void ImageView::fit(bool always) {
    if (!image()) return;
    double w, h; rotatedSize(w, h);
    double z = std::min(width() / w, height() / h);
    zoom_ = always ? z : std::min(1.0, z);
    offx_ = offy_ = 0; autoFit_ = true;
    update();
    emit statusChanged();
}

void ImageView::setZoom(double z, QPointF at) {
    z = std::clamp(z, 0.0005, 256.0);
    double cx = at.x() - width() / 2.0, cy = at.y() - height() / 2.0;
    offx_ = cx + (offx_ - cx) * z / zoom_;
    offy_ = cy + (offy_ - cy) * z / zoom_;
    zoom_ = z; autoFit_ = false;
    update();
    emit statusChanged();
}

void ImageView::zoomStep(int dir) { zoomStep(dir, QPointF(width() / 2.0, height() / 2.0)); }

void ImageView::zoomStep(int dir, QPointF at) {
    static const double steps[] = {0.01, 0.02, 0.03, 0.05, 0.0625, 0.083, 0.1, 0.125, 0.167, 0.25, 0.333, 0.5, 0.667, 0.75,
                                   1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
    double z = zoom_;
    if (dir > 0) { for (double s : steps) if (s > z * 1.001) { setZoom(s, at); return; } setZoom(z * 1.5, at); }
    else { for (int i = (int)std::size(steps) - 1; i >= 0; i--) if (steps[i] < z / 1.001) { setZoom(steps[i], at); return; } setZoom(z / 1.5, at); }
}

void ImageView::actualPixels() {
    int d = r_ ? r_->info.scale_div : 1;
    setZoom(1.0 * d, QPointF(width() / 2.0, height() / 2.0));
    offx_ = offy_ = 0;
    update();
}

void ImageView::fitWidth() {
    if (!image()) return;
    double w, h; rotatedSize(w, h);
    zoom_ = width() / w; offx_ = 0;
    offy_ = std::max(0.0, (h * zoom_ - height()) / 2);   // show the top
    autoFit_ = false;
    update();
    emit statusChanged();
}

void ImageView::rotate(int q) { rot_ = ((rot_ + q) % 4 + 4) % 4; if (autoFit_) fit(false); else update(); }
void ImageView::flipH() { flipH_ = !flipH_; update(); }
void ImageView::flipV() { flipV_ = !flipV_; update(); }
void ImageView::resetTransform() { rot_ = 0; flipH_ = flipV_ = false; if (autoFit_) fit(false); else update(); }

double ImageView::zoomPercent() const {
    int d = r_ && r_->info.scale_div > 0 ? r_->info.scale_div : 1;
    return zoom_ / d * 100;
}

QImage ImageView::transformedImage() const {
    QImage q = qimage_from_image(image());
    if (q.isNull()) return q;
    if (flipH_ || flipV_) q = q.mirrored(flipH_, flipV_);
    if (rot_) q = q.transformed(QTransform().rotate(rot_ * 90));
    return q;
}

// ---------------------------------------------------------------- painting
void ImageView::drawImageOnce(QPainter& p, double dx, double dy) {
    Image* im = image();
    QTransform m = QTransform::fromTranslate(dx, dy) * viewTransform();
    // coarsest mip level that still has at least screen resolution
    int L = 0;
    while (L + 1 < (int)im->lv.size() && im->lv[L + 1].w >= zoom_ * im->width()) L++;
    const Level& l = im->lv[L];
    double fx = (double)im->width() / l.w, fy = (double)im->height() / l.h;
    QTransform lm = QTransform::fromScale(fx, fy) * m;          // level px -> screen
    bool ok = false;
    QTransform inv = lm.inverted(&ok);
    if (!ok) return;
    QRectF vis = inv.mapRect(QRectF(rect()));
    int c0 = std::max(0, (int)std::floor(vis.left() / Image::T)), c1 = std::min(l.cols - 1, (int)std::floor(vis.right() / Image::T));
    int r0 = std::max(0, (int)std::floor(vis.top() / Image::T)), r1 = std::min(l.rows - 1, (int)std::floor(vis.bottom() / Image::T));
    if (c0 > c1 || r0 > r1) return;

    if (im->has_alpha) {                                         // checkerboard behind transparency
        p.setTransform(QTransform());
        p.setPen(Qt::NoPen);
        p.setBrush(checkerBrush());
        p.drawPolygon(lm.map(QRectF(0, 0, l.w, l.h)));
    }
    double scr = zoom_ * fx;                                     // screen px per level px
    p.setRenderHint(QPainter::SmoothPixmapTransform, scr < 1.0 || smooth);
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            Tile* t = l.tile(c, r);
            QImage q(t->data, t->w, t->h, t->stride, QImage::Format_ARGB32_Premultiplied);
            p.setTransform(QTransform::fromTranslate(c * Image::T, r * Image::T) * lm);
            p.drawImage(QPointF(0, 0), q);
        }
}

void ImageView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    switch (bg) {
    case BgDark: p.fillRect(rect(), QColor(28, 28, 31)); break;
    case BgGray: p.fillRect(rect(), QColor(128, 128, 128)); break;
    case BgWhite: p.fillRect(rect(), Qt::white); break;
    default: p.fillRect(rect(), checkerBrush());
    }
    if (!image()) {
        QString msg = message_;
        if (msg.isEmpty() && r_ && !r_->error.empty()) msg = "Cannot open this file:\n" + QString::fromStdString(r_->error);
        if (msg.isEmpty()) msg = "Open an image with Ctrl+O, or drop a file here.\nRight-click anywhere for all commands.";
        bool light = bg == BgWhite || bg == BgChecker;
        p.setPen(light ? QColor(50, 50, 50) : QColor(190, 190, 195));
        QFont f = font(); f.setPointSizeF(f.pointSizeF() * 1.2); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, msg);
        return;
    }
    Image* im = image();
    if (tiled) {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) drawImageOnce(p, dx * im->width(), dy * im->height());
    } else drawImageOnce(p, 0, 0);
}

void ImageView::resizeEvent(QResizeEvent*) { if (autoFit_ && image()) fit(zoom_ > 1.0001); }

// ---------------------------------------------------------------- input
void ImageView::mousePressEvent(QMouseEvent* e) {
    switch (e->button()) {
    case Qt::LeftButton: dragging_ = true; dragPos_ = e->position(); setCursor(Qt::ClosedHandCursor); break;
    case Qt::MiddleButton: fit(true); break;
    case Qt::RightButton: emit contextMenuRequested(e->globalPosition().toPoint()); break;
    case Qt::BackButton: emit navigate(-1); break;
    case Qt::ForwardButton: emit navigate(1); break;
    default: break;
    }
}

void ImageView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) { dragging_ = false; unsetCursor(); }
}

void ImageView::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) {
        QPointF d = e->position() - dragPos_;
        offx_ += d.x(); offy_ += d.y();
        dragPos_ = e->position();
        autoFit_ = false;
        update();
    }
    updatePixel(e->position());
}

void ImageView::mouseDoubleClickEvent(QMouseEvent* e) { if (e->button() == Qt::LeftButton) emit doubleClicked(); }

void ImageView::wheelEvent(QWheelEvent* e) {
    static int accum = 0;
    accum += e->angleDelta().y();
    if (std::abs(accum) < 120) return;
    int dir = accum > 0 ? 1 : -1;
    accum = 0;
    bool zoom = wheelZoom != bool(e->modifiers() & Qt::ControlModifier);    // Ctrl inverts
    if (zoom) zoomStep(dir, e->position()); else emit navigate(-dir);
}

void ImageView::updatePixel(QPointF pos) {
    pixel_.clear();
    if (Image* im = image()) {
        bool ok = false;
        QPointF ip = viewTransform().inverted(&ok).map(pos);
        uint8_t px[4];
        if (ok && im->pixel((int)std::floor(ip.x()), (int)std::floor(ip.y()), px)) {
            int d = r_->info.scale_div;
            pixel_ = QString("%1, %2   #%3%4%5").arg((int)std::floor(ip.x()) * d).arg((int)std::floor(ip.y()) * d)
                         .arg(px[0], 2, 16, QChar('0')).arg(px[1], 2, 16, QChar('0')).arg(px[2], 2, 16, QChar('0')).toUpper();
            if (im->has_alpha) pixel_ += QString("  a%1").arg(px[3]);
        }
    }
    emit statusChanged();
}
