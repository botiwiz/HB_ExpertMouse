#include "modelview.h"
#include <f3d/engine.h>
#include <f3d/image.h>
#include <f3d/interactor.h>
#include <f3d/options.h>
#include <f3d/scene.h>
#include <f3d/window.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <vector>

static const QColor kBg(28, 28, 31);

ModelView::ModelView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    frameTimer_ = new QTimer(this);
    frameTimer_->setSingleShot(true);
    frameTimer_->setInterval(0);
    connect(frameTimer_, &QTimer::timeout, this, &ModelView::renderFrame);
    tick_ = new QTimer(this);                              // f3d animations, notifications, smooth camera
    tick_->setInterval(33);
    connect(tick_, &QTimer::timeout, this, [this] {
        if (!engine_ || !isVisible()) return;
        try { inter_->triggerEventLoop(1.0 / 30.0); } catch (...) {}
        if (animating_ || QDateTime::currentMSecsSinceEpoch() < busyUntil_) requestFrame();
    });
}

ModelView::~ModelView() = default;

QString ModelView::f3dVersion() {
    try { return QString::fromStdString(f3d::engine::getLibInfo().VersionFull); } catch (...) { return {}; }
}

bool ModelView::ensureEngine() {
    if (engine_) return true;
    try {
        f3d::engine::autoloadPlugins();
        QString app = QCoreApplication::applicationDirPath();
        std::vector<std::filesystem::path> dirs = {(app + "/../lib").toStdString(), (app + "/../lib/f3d").toStdString(), app.toStdString()};
        for (const char* p : {"assimp", "occt", "alembic", "draco", "usd", "hdf", "vdb"}) {
            try { f3d::engine::loadPlugin(p, dirs); } catch (...) {}
        }
        engine_ = std::make_unique<f3d::engine>(f3d::engine::create(true));   // offscreen, own GL context
        inter_ = &engine_->getInteractor();
    } catch (const std::exception& e) {
        error_ = QString("Could not start f3d: ") + e.what();
        engine_.reset();
        inter_ = nullptr;
        return false;
    }
    f3d::options& o = engine_->getOptions();
    auto set = [&o](const char* name, const char* value) { try { o.setAsString(name, value); } catch (...) {} };
    set("render.background.color", "0.11,0.11,0.12");      // matches the image viewer
    set("render.grid.enable", "true");
    set("ui.axis", "true");
    set("ui.filename", "false");                           // the status bar shows it
    set("render.effect.tone_mapping", "true");
    set("render.hdri.ambient", "false");
    syncSize();
    tick_->start();
    return true;
}

void ModelView::syncSize() {
    if (!engine_) return;
    qreal dpr = devicePixelRatioF();
    engine_->getWindow().setSize(std::max(1, int(width() * dpr)), std::max(1, int(height() * dpr)));
}

void ModelView::load(const QString& path) {
    error_.clear();
    if (!ensureEngine()) { update(); emit loaded(false); return; }
    bool ok = true;
    try {
        f3d::scene& sc = engine_->getScene();
        sc.clear();
        sc.add(std::filesystem::path(path.toStdString()));
        engine_->getWindow().getCamera().resetToBounds();
        path_ = path;
    } catch (const std::exception& e) {
        error_ = QString::fromLocal8Bit(e.what());
        ok = false;
    }
    animating_ = false;
    renderFrame();                                          // first frame right away
    emit loaded(ok);
}

void ModelView::unload() {
    path_.clear();
    frame_ = QImage();
    animating_ = false;
    if (engine_) { try { engine_->getScene().clear(); } catch (...) {} }
    update();
}

void ModelView::command(const QString& cmd) {
    if (!engine_) return;
    if (cmd == "toggle_animation") animating_ = !animating_;
    try { inter_->triggerCommand(cmd.toStdString()); } catch (...) {}
    busy();
    requestFrame();
}

void ModelView::busy() { busyUntil_ = QDateTime::currentMSecsSinceEpoch() + 800; }

void ModelView::requestFrame() { if (!frameTimer_->isActive()) frameTimer_->start(); }

// One complete frame from f3d's offscreen window; the widget never sees partial renders.
void ModelView::renderFrame() {
    if (!engine_ || !isVisible()) return;
    try {
        f3d::image img = engine_->getWindow().renderToImage();
        int w = (int)img.getWidth(), h = (int)img.getHeight(), ch = (int)img.getChannelCount();
        if (w > 0 && h > 0 && (ch == 3 || ch == 4) && img.getChannelType() == f3d::image::ChannelType::BYTE) {
            QImage q(w, h, ch == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
            const uint8_t* src = static_cast<const uint8_t*>(img.getContent());
            size_t row = (size_t)w * ch;
            for (int y = 0; y < h; y++) memcpy(q.scanLine(y), src + (size_t)(h - 1 - y) * row, row);   // bottom-up -> top-down
            q.setDevicePixelRatio(devicePixelRatioF());
            frame_ = q;
        }
    } catch (const std::exception& e) {
        error_ = QString::fromLocal8Bit(e.what());
    }
    update();
}

void ModelView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kBg);
    if (!frame_.isNull()) p.drawImage(QPointF(0, 0), frame_);
    if (!error_.isEmpty() && path_.isEmpty()) {
        p.setPen(QColor(190, 190, 195));
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, error_);
    }
}

void ModelView::resizeEvent(QResizeEvent*) { syncSize(); requestFrame(); }
void ModelView::showEvent(QShowEvent*) { syncSize(); requestFrame(); }

// ---------------------------------------------------------------- input -> f3d
void ModelView::mods(Qt::KeyboardModifiers m) {
    using M = f3d::interactor::InputModifier;
    bool c = m & Qt::ControlModifier, s = m & Qt::ShiftModifier;
    inter_->triggerModUpdate(c && s ? M::CTRL_SHIFT : c ? M::CTRL : s ? M::SHIFT : M::NONE);
}

static bool mapButton(Qt::MouseButton b, f3d::interactor::MouseButton& out) {
    using B = f3d::interactor::MouseButton;
    switch (b) {
    case Qt::LeftButton: out = B::LEFT; return true;
    case Qt::RightButton: out = B::RIGHT; return true;
    case Qt::MiddleButton: out = B::MIDDLE; return true;
    default: return false;
    }
}

void ModelView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) { rightDown_ = true; pressPos_ = e->globalPosition().toPoint(); }
    f3d::interactor::MouseButton b;
    if (!inter_ || !mapButton(e->button(), b)) return QWidget::mousePressEvent(e);
    qreal dpr = devicePixelRatioF();
    try {
        inter_->triggerMousePosition(e->position().x() * dpr, e->position().y() * dpr);
        mods(e->modifiers());
        inter_->triggerMouseButton(f3d::interactor::InputAction::PRESS, b);
    } catch (...) {}
    requestFrame();
}

void ModelView::mouseMoveEvent(QMouseEvent* e) {
    if (!inter_) return;
    qreal dpr = devicePixelRatioF();
    try {
        inter_->triggerMousePosition(e->position().x() * dpr, e->position().y() * dpr);
        mods(e->modifiers());
    } catch (...) {}
    if (e->buttons()) requestFrame();                        // only dragging changes the view
}

void ModelView::mouseReleaseEvent(QMouseEvent* e) {
    f3d::interactor::MouseButton b;
    if (inter_ && mapButton(e->button(), b)) {
        qreal dpr = devicePixelRatioF();
        try {
            inter_->triggerMousePosition(e->position().x() * dpr, e->position().y() * dpr);
            mods(e->modifiers());
            inter_->triggerMouseButton(f3d::interactor::InputAction::RELEASE, b);
        } catch (...) {}
        busy();
        requestFrame();
    }
    // right-click without dragging = our command menu (right-drag stays f3d's zoom)
    if (e->button() == Qt::RightButton && rightDown_) {
        rightDown_ = false;
        if ((e->globalPosition().toPoint() - pressPos_).manhattanLength() < 4) emit contextMenuRequested(pressPos_);
    }
}

void ModelView::wheelEvent(QWheelEvent* e) {
    if (!inter_) return;
    using W = f3d::interactor::WheelDirection;
    QPoint d = e->angleDelta();
    W dir = std::abs(d.y()) >= std::abs(d.x()) ? (d.y() > 0 ? W::FORWARD : W::BACKWARD) : (d.x() > 0 ? W::RIGHT : W::LEFT);
    try { mods(e->modifiers()); inter_->triggerMouseWheel(dir); } catch (...) {}
    busy();
    requestFrame();
}

static std::string keySym(QKeyEvent* e) {
    int k = e->key();
    if (k >= Qt::Key_A && k <= Qt::Key_Z) return std::string(1, char('A' + (k - Qt::Key_A)));
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12) return "F" + std::to_string(k - Qt::Key_F1 + 1);
    switch (k) {
    case Qt::Key_Backspace: return "BackSpace";
    case Qt::Key_Escape: return "Escape";
    case Qt::Key_Return: case Qt::Key_Enter: return "Return";
    case Qt::Key_Space: return "Space";
    case Qt::Key_Tab: return "Tab";
    case Qt::Key_Up: return "Up";
    case Qt::Key_Down: return "Down";
    case Qt::Key_Left: return "Left";
    case Qt::Key_Right: return "Right";
    case Qt::Key_Delete: return "Delete";
    case Qt::Key_Shift: return "Shift_L";
    case Qt::Key_Control: return "Control_L";
    case Qt::Key_Alt: return "Alt_L";
    case Qt::Key_Meta: return "Super_L";
    default: break;
    }
    return e->text().toStdString();
}

void ModelView::keyPressEvent(QKeyEvent* e) {
    if (!inter_) return QWidget::keyPressEvent(e);
    try {
        mods(e->modifiers());
        std::string ks = keySym(e);
        if (!ks.empty()) inter_->triggerKeyboardKey(f3d::interactor::InputAction::PRESS, ks);
        for (char16_t ch : e->text().toStdU16String()) inter_->triggerTextCharacter(ch);
    } catch (...) {}
    if (e->key() == Qt::Key_Space) animating_ = !animating_;   // f3d's default play/pause key
    busy();
    requestFrame();
}

void ModelView::keyReleaseEvent(QKeyEvent* e) {
    if (!inter_) return QWidget::keyReleaseEvent(e);
    try {
        mods(e->modifiers());
        std::string ks = keySym(e);
        if (!ks.empty()) inter_->triggerKeyboardKey(f3d::interactor::InputAction::RELEASE, ks);
    } catch (...) {}
    requestFrame();
}
