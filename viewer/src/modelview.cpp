#include "modelview.h"
#include <f3d/engine.h>
#include <f3d/interactor.h>
#include <f3d/options.h>
#include <f3d/scene.h>
#include <f3d/window.h>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <vector>

// f3d's external window first copies the bound framebuffer into its own and composites on
// top of it (FrameBlitMode BlitToCurrent). Our FBO still holds the previous frame, which then
// ghosts through, and renders triggered by input outside paintGL could surface stale frames.
// So every f3d render starts from a freshly cleared framebuffer.
void ModelView::beginGL() {
    makeCurrent();
    QOpenGLFunctions* f = context()->functions();
    f->glClearColor(0.11f, 0.11f, 0.12f, 1.0f);
    f->glClearDepthf(1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

ModelView::ModelView(QWidget* parent) : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

ModelView::~ModelView() {
    makeCurrent();            // f3d releases GL resources in its destructor
    engine_.reset();
    doneCurrent();
}

QString ModelView::f3dVersion() {
    try { return QString::fromStdString(f3d::engine::getLibInfo().VersionFull); } catch (...) { return {}; }
}

void ModelView::initializeGL() {
    try {
        f3d::engine::autoloadPlugins();
        // the dynamic plugins of the bundled f3d (assimp, occt, usd, alembic, draco...), when present
        QString app = QCoreApplication::applicationDirPath();
        std::vector<std::filesystem::path> dirs = {(app + "/../lib").toStdString(), (app + "/../lib/f3d").toStdString(), app.toStdString()};
        for (const char* p : {"assimp", "occt", "alembic", "draco", "usd", "hdf", "vdb"}) {
            try { f3d::engine::loadPlugin(p, dirs); } catch (...) {}
        }
        f3d::context::function load = [this](const char* name) { return context()->getProcAddress(name); };
        engine_ = std::make_unique<f3d::engine>(f3d::engine::createExternal(load));
    } catch (const std::exception& e) {
        error_ = QString("Could not start f3d: ") + e.what();
        engine_.reset();
        return;
    }
    inter_ = &engine_->getInteractor();
    f3d::options& o = engine_->getOptions();
    auto set = [&o](const char* name, const char* value) { try { o.setAsString(name, value); } catch (...) {} };
    set("render.background.color", "0.11,0.11,0.12");    // matches the image viewer
    set("render.grid.enable", "true");
    set("ui.axis", "true");
    set("ui.filename", "false");                         // the status bar shows it
    set("render.effect.tone_mapping", "true");
    set("render.hdri.ambient", "false");
    engine_->getWindow().setSize(std::max(1, width()), std::max(1, height()));

    tick_ = new QTimer(this);                            // f3d animations, notifications, smooth camera
    tick_->setInterval(30);
    connect(tick_, &QTimer::timeout, this, [this] {
        if (!engine_ || !isVisible()) return;
        beginGL();
        inter_->triggerEventLoop(1.0 / 30.0);
        doneCurrent();
        update();
    });
    tick_->start();
    if (!pending_.isEmpty()) doLoad();
}

void ModelView::load(const QString& path) {
    pending_ = path;
    if (engine_) doLoad();
    else update();                                       // initializeGL will pick it up
}

void ModelView::unload() {
    pending_.clear(); path_.clear();
    if (!engine_) return;
    makeCurrent();
    try { engine_->getScene().clear(); } catch (...) {}
    doneCurrent();
    update();
}

void ModelView::doLoad() {
    QString p = pending_;
    pending_.clear();
    error_.clear();
    bool ok = true;
    beginGL();
    try {
        f3d::scene& sc = engine_->getScene();
        sc.clear();
        sc.add(std::filesystem::path(p.toStdString()));
        engine_->getWindow().getCamera().resetToBounds();
        path_ = p;
    } catch (const std::exception& e) {
        error_ = QString::fromLocal8Bit(e.what());
        ok = false;
    }
    doneCurrent();
    update();
    emit loaded(ok);
}

void ModelView::command(const QString& cmd) {
    if (!engine_) return;
    beginGL();
    try { inter_->triggerCommand(cmd.toStdString()); } catch (...) {}
    doneCurrent();
    update();
}

void ModelView::resizeGL(int w, int h) {
    if (engine_) engine_->getWindow().setSize(std::max(1, w), std::max(1, h));
}

void ModelView::paintGL() {
    QOpenGLFunctions* f = context()->functions();      // already current inside paintGL
    f->glClearColor(0.11f, 0.11f, 0.12f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (engine_) { try { engine_->getWindow().render(); } catch (...) {} }
}

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
    if (!inter_ || !mapButton(e->button(), b)) return QOpenGLWidget::mousePressEvent(e);
    beginGL();
    inter_->triggerMousePosition(e->position().x(), e->position().y());
    mods(e->modifiers());
    inter_->triggerMouseButton(f3d::interactor::InputAction::PRESS, b);
    doneCurrent();
    update();
}

void ModelView::mouseMoveEvent(QMouseEvent* e) {
    if (!inter_) return;
    beginGL();
    inter_->triggerMousePosition(e->position().x(), e->position().y());
    mods(e->modifiers());
    doneCurrent();
    update();
}

void ModelView::mouseReleaseEvent(QMouseEvent* e) {
    f3d::interactor::MouseButton b;
    if (inter_ && mapButton(e->button(), b)) {
        beginGL();
        inter_->triggerMousePosition(e->position().x(), e->position().y());
        mods(e->modifiers());
        inter_->triggerMouseButton(f3d::interactor::InputAction::RELEASE, b);
        doneCurrent();
        update();
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
    beginGL();
    mods(e->modifiers());
    inter_->triggerMouseWheel(dir);
    doneCurrent();
    update();
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
    if (!inter_) return QOpenGLWidget::keyPressEvent(e);
    beginGL();
    mods(e->modifiers());
    std::string ks = keySym(e);
    if (!ks.empty()) inter_->triggerKeyboardKey(f3d::interactor::InputAction::PRESS, ks);
    for (char16_t ch : e->text().toStdU16String()) inter_->triggerTextCharacter(ch);
    doneCurrent();
    update();
}

void ModelView::keyReleaseEvent(QKeyEvent* e) {
    if (!inter_) return QOpenGLWidget::keyReleaseEvent(e);
    beginGL();
    mods(e->modifiers());
    std::string ks = keySym(e);
    if (!ks.empty()) inter_->triggerKeyboardKey(f3d::interactor::InputAction::RELEASE, ks);
    doneCurrent();
    update();
}
