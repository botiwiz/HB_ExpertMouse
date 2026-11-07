#include "imageview.h"
#include <QMouseEvent>
#include <QMovie>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>


ImageView::ImageView(QWidget* parent) : QOpenGLWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

ImageView::~ImageView() {
    if (!context()) return;
    makeCurrent();
    dropTextures();
    delete prog_;
    doneCurrent();
}

void ImageView::clear() {
    if (movie_) { movie_->deleteLater(); movie_ = nullptr; }
    r_.reset();
    ++imgGen_;
    path_.clear();
    pixel_.clear();
    update();
}

void ImageView::setMessage(const QString& msg) { message_ = msg; update(); }

void ImageView::setResult(std::shared_ptr<LoadResult> r, const QString& path, bool keepView) {
    double oldW = keepView && image() ? image()->width() : 0;
    if (movie_) { movie_->deleteLater(); movie_ = nullptr; }
    r_ = std::move(r);
    ++imgGen_;
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
        if (auto im = image_from_qimage(movie_->currentImage())) { im->build_mips(); r_->img = std::move(im); ++imgGen_; }
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

// ---------------------------------------------------------------- painting (GPU)
static const char* kVert = R"(
attribute vec2 pos;          // unit quad 0..1
uniform mat4 mvp;
uniform vec2 size;           // quad size in its own pixels
varying vec2 uv;
void main() { uv = pos; gl_Position = mvp * vec4(pos * size, 0.0, 1.0); }
)";
static const char* kFrag = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec2 uv;
uniform sampler2D tex;
uniform int mode;            // 0 = tile (premultiplied BGRA bytes), 1 = checkerboard
uniform float dpr;
void main() {
    if (mode == 1) {
        vec2 c = floor(gl_FragCoord.xy / (8.0 * dpr));
        float k = mod(c.x + c.y, 2.0);
        gl_FragColor = vec4(vec3(mix(0.8, 0.6, k)), 1.0);
    } else {
        gl_FragColor = texture2D(tex, uv).bgra;   // tiles are stored as B,G,R,A bytes
    }
}
)";

void ImageView::initializeGL() {
    initializeOpenGLFunctions();
    prog_ = new QOpenGLShaderProgram;
    prog_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
    prog_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
    prog_->bindAttributeLocation("pos", 0);
    prog_->link();
    glRenderer = QString::fromLatin1(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    softwareGL = glRenderer.contains("llvmpipe", Qt::CaseInsensitive) || glRenderer.contains("softpipe", Qt::CaseInsensitive) ||
                 glRenderer.contains("swrast", Qt::CaseInsensitive);
}

void ImageView::dropTextures() {
    for (auto& t : tex_) glDeleteTextures(1, &t.second.id);
    tex_.clear();
    texBytes_ = 0;
}

unsigned ImageView::textureFor(int level, int c, int r) {
    quint64 key = (quint64(level) << 48) | (quint64(c) << 24) | quint64(r);
    auto it = tex_.find(key);
    if (it != tex_.end()) { it->second.used = frameNo_; return it->second.id; }
    Tile* t = image()->lv[level].tile(c, r);
    size_t bytes = size_t(t->w) * t->h * 4;
    // evict least recently used tiles (never ones drawn in this frame) to stay in the VRAM budget
    while (texBytes_ + bytes > texBudget_ && !tex_.empty()) {
        auto old = tex_.begin();
        for (auto i = tex_.begin(); i != tex_.end(); ++i) if (i->second.used < old->second.used) old = i;
        if (old->second.used == frameNo_) break;
        glDeleteTextures(1, &old->second.id);
        texBytes_ -= old->second.bytes;
        tex_.erase(old);
    }
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // upload the B,G,R,A bytes as-is (RGBA texture); the shader swizzles - works on GL and GLES
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    tex_[key] = {id, bytes, frameNo_};
    texBytes_ += bytes;
    return id;
}

void ImageView::drawQuad(const QTransform& toScreen, double w, double h, int mode) {
    QMatrix4x4 proj;
    proj.ortho(0, width(), height(), 0, -1, 1);                // widget px, y down
    prog_->setUniformValue("mvp", proj * QMatrix4x4(toScreen));
    prog_->setUniformValue("size", QVector2D(w, h));
    prog_->setUniformValue("mode", mode);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void ImageView::drawImageOnce(double dx, double dy) {
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

    if (im->has_alpha) drawQuad(lm, l.w, l.h, 1);              // checkerboard behind transparency
    double scr = zoom_ * fx;                                     // screen px per level px
    GLint filter = (scr < 1.0 || smooth) ? GL_LINEAR : GL_NEAREST;
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            Tile* t = l.tile(c, r);
            glBindTexture(GL_TEXTURE_2D, textureFor(L, c, r));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            drawQuad(QTransform::fromTranslate(c * Image::T, r * Image::T) * lm, t->w, t->h, 0);
        }
}

void ImageView::paintGL() {
    ++frameNo_;
    QColor bgc = bg == BgGray ? QColor(128, 128, 128) : bg == BgWhite ? QColor(Qt::white) : QColor(28, 28, 31);
    glClearColor(bgc.redF(), bgc.greenF(), bgc.blueF(), 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (texGen_ != imgGen_) { dropTextures(); texGen_ = imgGen_; }       // new image / animation frame

    static const GLfloat quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    prog_->bind();
    prog_->enableAttributeArray(0);
    prog_->setAttributeArray(0, GL_FLOAT, quad, 2);
    prog_->setUniformValue("tex", 0);
    prog_->setUniformValue("dpr", float(devicePixelRatioF()));
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);                // premultiplied alpha
    if (bg == BgChecker) drawQuad(QTransform(), width(), height(), 1);
    if (Image* im = image()) {
        if (tiled) {
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) drawImageOnce(dx * im->width(), dy * im->height());
        } else drawImageOnce(0, 0);
    }
    prog_->disableAttributeArray(0);
    prog_->release();
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!image()) {                                              // message text (QPainter, also GPU-backed here)
        QPainter p(this);
        QString msg = message_;
        if (msg.isEmpty() && r_ && !r_->error.empty()) msg = "Cannot open this file:\n" + QString::fromStdString(r_->error);
        if (msg.isEmpty()) msg = "Open an image with Ctrl+O, or drop a file here.\nRight-click anywhere for all commands.";
        bool light = bg == BgWhite || bg == BgChecker;
        p.setPen(light ? QColor(50, 50, 50) : QColor(190, 190, 195));
        QFont f = font(); f.setPointSizeF(f.pointSizeF() * 1.2); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, msg);
    }
}

void ImageView::resizeEvent(QResizeEvent* e) {
    QOpenGLWidget::resizeEvent(e);                  // resizes the framebuffer (else the old one gets stretched)
    if (autoFit_ && image()) fit(zoom_ > 1.0001);
}

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
