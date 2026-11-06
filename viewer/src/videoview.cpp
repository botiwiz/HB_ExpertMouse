#include "videoview.h"
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <QApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLibrary>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <clocale>
#include <cmath>
#include <type_traits>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------- libmpv, loaded at runtime
namespace {
struct Mpv {
    bool ok = false;
    decltype(&mpv_create) create;
    decltype(&mpv_initialize) initialize;
    decltype(&mpv_terminate_destroy) terminate_destroy;
    decltype(&mpv_set_option_string) set_option_string;
    decltype(&mpv_set_property_string) set_property_string;
    decltype(&mpv_get_property_string) get_property_string;
    decltype(&mpv_free) free;
    decltype(&mpv_command_async) command_async;
    decltype(&mpv_observe_property) observe_property;
    decltype(&mpv_wait_event) wait_event;
    decltype(&mpv_set_wakeup_callback) set_wakeup_callback;
    decltype(&mpv_error_string) error_string;
    decltype(&mpv_render_context_create) render_create;
    decltype(&mpv_render_context_render) render_render;
    decltype(&mpv_render_context_set_update_callback) render_set_update_callback;
    decltype(&mpv_render_context_free) render_free;
};

Mpv& api() {
    static Mpv m = [] {
        Mpv a;
        static QLibrary lib;
        for (const char* name : {"libmpv.so.2", "libmpv.so.1", "libmpv.so"}) {
            lib.setFileName(QString::fromLatin1(name));
            if (lib.load()) break;
        }
        if (!lib.isLoaded()) return a;
        bool all = true;
        auto get = [&](auto& fn, const char* sym) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(lib.resolve(sym));
            if (!fn) all = false;
        };
        get(a.create, "mpv_create");
        get(a.initialize, "mpv_initialize");
        get(a.terminate_destroy, "mpv_terminate_destroy");
        get(a.set_option_string, "mpv_set_option_string");
        get(a.set_property_string, "mpv_set_property_string");
        get(a.get_property_string, "mpv_get_property_string");
        get(a.free, "mpv_free");
        get(a.command_async, "mpv_command_async");
        get(a.observe_property, "mpv_observe_property");
        get(a.wait_event, "mpv_wait_event");
        get(a.set_wakeup_callback, "mpv_set_wakeup_callback");
        get(a.error_string, "mpv_error_string");
        get(a.render_create, "mpv_render_context_create");
        get(a.render_render, "mpv_render_context_render");
        get(a.render_set_update_callback, "mpv_render_context_set_update_callback");
        get(a.render_free, "mpv_render_context_free");
        a.ok = all;
        return a;
    }();
    return m;
}

void* glProc(void*, const char* name) {
    QOpenGLContext* c = QOpenGLContext::currentContext();
    return c ? reinterpret_cast<void*>(c->getProcAddress(QByteArray(name))) : nullptr;
}

QString fmtTime(double s) {
    if (s < 0 || s != s) s = 0;
    int t = (int)s, h = t / 3600, m = t / 60 % 60, sec = t % 60;
    return h ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'))
             : QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}
} // namespace

// ---------------------------------------------------------------- MpvWidget
MpvWidget::MpvWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    Mpv& m = api();
    if (!m.ok) { error_ = "libmpv was not found (install the 'mpv' or 'libmpv2' package)."; return; }
    std::setlocale(LC_NUMERIC, "C");                    // required by libmpv
    mpv_ = m.create();
    if (!mpv_) { error_ = "mpv_create failed"; return; }
    m.set_option_string(mpv_, "vo", "libmpv");
    QByteArray hw = qgetenv("HBV_HWDEC");                     // diagnostics override
    m.set_option_string(mpv_, "hwdec", hw.isEmpty() ? "auto" : hw.constData());   // GPU decoding first
    // diagnostics: extra options, e.g. HBV_MPV_OPTS="gpu-dumb-mode=yes,fbo-format=rgba8"
    for (const QByteArray& kv : qgetenv("HBV_MPV_OPTS").split(','))
        if (int eq = kv.indexOf('='); eq > 0) m.set_option_string(mpv_, kv.left(eq).constData(), kv.mid(eq + 1).constData());
    m.set_option_string(mpv_, "keep-open", "yes");    // stay on the last frame at the end
    m.set_option_string(mpv_, "idle", "yes");
    if (qEnvironmentVariableIsSet("HBV_MPV_LOG")) {          // diagnostics: mpv's own log on stderr
        m.set_option_string(mpv_, "terminal", "yes");
        m.set_option_string(mpv_, "msg-level", "all=v");
    } else m.set_option_string(mpv_, "terminal", "no");
    m.set_option_string(mpv_, "input-default-bindings", "no");
    m.set_option_string(mpv_, "osc", "no");
    m.set_option_string(mpv_, "ytdl", "no");
    if (int err = m.initialize(mpv_); err < 0) {
        error_ = QString("mpv_initialize: ") + m.error_string(err);
        m.terminate_destroy(mpv_);
        mpv_ = nullptr;
        return;
    }
    m.observe_property(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE);
    m.observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
    m.observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);
    m.observe_property(mpv_, 0, "volume", MPV_FORMAT_DOUBLE);
    m.observe_property(mpv_, 0, "mute", MPV_FORMAT_FLAG);
    m.set_wakeup_callback(mpv_, [](void* ctx) {
        auto* self = static_cast<MpvWidget*>(ctx);
        QMetaObject::invokeMethod(self, [self] { self->drainEvents(); }, Qt::QueuedConnection);
    }, this);
}

MpvWidget::~MpvWidget() {
    Mpv& m = api();
    if (rctx_) { makeCurrent(); m.render_free(rctx_); rctx_ = nullptr; doneCurrent(); }
    if (mpv_) { m.set_wakeup_callback(mpv_, nullptr, nullptr); m.terminate_destroy(mpv_); mpv_ = nullptr; }
}

void MpvWidget::initializeGL() {
    if (!mpv_) return;
    mpv_opengl_init_params gl{glProc, nullptr};
    std::vector<mpv_render_param> params = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl}};
    // the display connection lets GPU-decoded frames (VA-API...) go straight to the screen
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (auto* x = qApp->nativeInterface<QNativeInterface::QX11Application>())
        params.push_back({MPV_RENDER_PARAM_X11_DISPLAY, x->display()});
    if (auto* w = qApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        params.push_back({MPV_RENDER_PARAM_WL_DISPLAY, w->display()});
#endif
    params.push_back({MPV_RENDER_PARAM_INVALID, nullptr});
    // software OpenGL (llvmpipe/softpipe/swrast, i.e. no GPU driver): mpv's float FBOs render
    // unreliably there, so use 8-bit ones, and tell the user
    if (auto* f = context()->functions()) {
        QString r = QString::fromLatin1(reinterpret_cast<const char*>(f->glGetString(GL_RENDERER)));
        softwareGL = r.contains("llvmpipe", Qt::CaseInsensitive) || r.contains("softpipe", Qt::CaseInsensitive) ||
                     r.contains("swrast", Qt::CaseInsensitive);
        renderer = r;
        if (softwareGL) api().set_property_string(mpv_, "fbo-format", "rgba8");
    }
    if (int err = api().render_create(&rctx_, mpv_, params.data()); err < 0) {
        error_ = QString("mpv render context: ") + api().error_string(err);
        rctx_ = nullptr;
        return;
    }
    api().render_set_update_callback(rctx_, [](void* ctx) {
        auto* self = static_cast<MpvWidget*>(ctx);
        QMetaObject::invokeMethod(self, [self] { self->updatesRequested++; self->update(); }, Qt::QueuedConnection);
    }, this);
}

void MpvWidget::paintGL() {
    paints++;
    if (!rctx_) return;
    renders++;
    qreal dpr = devicePixelRatioF();
    mpv_opengl_fbo fbo{(int)defaultFramebufferObject(), int(width() * dpr), int(height() * dpr), 0};
    int flip = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_INVALID, nullptr}};
    api().render_render(rctx_, params);
}

void MpvWidget::drainEvents() {
    if (!mpv_) return;
    bool any = false;
    for (;;) {
        mpv_event* ev = api().wait_event(mpv_, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* p = static_cast<mpv_event_property*>(ev->data);
            if (!p->data) continue;
            if (!strcmp(p->name, "time-pos") && p->format == MPV_FORMAT_DOUBLE) position = *static_cast<double*>(p->data);
            else if (!strcmp(p->name, "duration") && p->format == MPV_FORMAT_DOUBLE) duration = *static_cast<double*>(p->data);
            else if (!strcmp(p->name, "volume") && p->format == MPV_FORMAT_DOUBLE) volume = *static_cast<double*>(p->data);
            else if (!strcmp(p->name, "pause") && p->format == MPV_FORMAT_FLAG) paused = *static_cast<int*>(p->data);
            else if (!strcmp(p->name, "mute") && p->format == MPV_FORMAT_FLAG) muted = *static_cast<int*>(p->data);
            any = true;
        } else if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            emit fileLoaded();
            any = true;
        }
    }
    if (any) emit changed();
}

void MpvWidget::command(const QStringList& args) {
    if (!mpv_) return;
    std::vector<QByteArray> store;
    std::vector<const char*> argv;
    for (const QString& a : args) store.push_back(a.toUtf8());
    for (const QByteArray& b : store) argv.push_back(b.constData());
    argv.push_back(nullptr);
    api().command_async(mpv_, 0, argv.data());
}

void MpvWidget::setProp(const char* name, const QString& value) {
    if (mpv_) api().set_property_string(mpv_, name, value.toUtf8().constData());
}

QString MpvWidget::propString(const char* name) const {
    if (!mpv_) return {};
    char* s = api().get_property_string(mpv_, name);
    if (!s) return {};
    QString r = QString::fromUtf8(s);
    api().free(s);
    return r;
}

// single click = play/pause (delayed so a double-click can toggle full screen instead)
void MpvWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) emit menuRequested(e->globalPosition().toPoint());
}
void MpvWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (clickPending_) return;
    clickPending_ = true;
    QTimer::singleShot(QApplication::doubleClickInterval(), this, [this] {
        if (clickPending_) { clickPending_ = false; emit clicked(); }
    });
}
void MpvWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    clickPending_ = false;
    emit doubleClicked();
}
void MpvWidget::wheelEvent(QWheelEvent* e) {
    int steps = e->angleDelta().y() / 120;
    if (steps) emit wheel(steps);
}

// ---------------------------------------------------------------- VideoView
bool VideoView::available() { return api().ok; }

VideoView::VideoView(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    mpv_ = new MpvWidget(this);
    lay->addWidget(mpv_, 1);

    auto* bar = new QWidget(this);
    bar->setObjectName("videobar");
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(8, 4, 8, 4);
    auto tool = [bar](const char* icon, const QString& tip) {
        auto* b = new QToolButton(bar);
        b->setIcon(QIcon::fromTheme(QString::fromLatin1(icon)));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    play_ = tool("media-playback-pause", "Play / pause (Space)");
    seek_ = new QSlider(Qt::Horizontal, bar);
    seek_->setFocusPolicy(Qt::NoFocus);
    seek_->setRange(0, 0);
    time_ = new QLabel("0:00 / 0:00", bar);
    mute_ = tool("audio-volume-high", "Mute (M)");
    vol_ = new QSlider(Qt::Horizontal, bar);
    vol_->setFocusPolicy(Qt::NoFocus);
    vol_->setRange(0, 130);
    vol_->setFixedWidth(110);
    full_ = tool("view-fullscreen", "Full screen (F)");
    bl->addWidget(play_);
    bl->addWidget(seek_, 1);
    bl->addWidget(time_);
    bl->addSpacing(8);
    bl->addWidget(mute_);
    bl->addWidget(vol_);
    bl->addWidget(full_);
    lay->addWidget(bar);

    connect(play_, &QToolButton::clicked, this, &VideoView::togglePause);
    connect(mute_, &QToolButton::clicked, this, &VideoView::toggleMute);
    connect(full_, &QToolButton::clicked, this, &VideoView::fullscreenRequested);
    connect(seek_, &QSlider::sliderPressed, this, [this] { seeking_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, [this] { seeking_ = false; seek(seek_->value() / 10.0, false); });
    connect(seek_, &QSlider::sliderMoved, this, [this](int v) { seek(v / 10.0, false); });
    connect(vol_, &QSlider::valueChanged, this, [this](int v) {
        if (std::abs(v - mpv_->volume) >= 1) mpv_->setProp("volume", QString::number(v)); });
    connect(mpv_, &MpvWidget::changed, this, [this] { refreshControls(); emit statusChanged(); });
    connect(mpv_, &MpvWidget::fileLoaded, this, [this] { emit statusChanged(); });
    connect(mpv_, &MpvWidget::clicked, this, &VideoView::togglePause);
    connect(mpv_, &MpvWidget::doubleClicked, this, &VideoView::fullscreenRequested);
    connect(mpv_, &MpvWidget::wheel, this, [this](int s) { addVolume(5.0 * s); });
    connect(mpv_, &MpvWidget::menuRequested, this, &VideoView::contextMenuRequested);
}

QString VideoView::error() const { return mpv_ ? mpv_->error() : QString("no player"); }

void VideoView::open(const QString& path) {
    path_ = path;
    mpv_->position = mpv_->duration = 0;
    mpv_->command({"loadfile", path});
    mpv_->setProp("pause", "no");
    mpv_->setFocus();
    refreshControls();
}

void VideoView::stop() {
    if (path_.isEmpty()) return;
    mpv_->command({"stop"});
    path_.clear();
}

void VideoView::togglePause() { mpv_->command({"cycle", "pause"}); }
void VideoView::seek(double s, bool relative) {
    mpv_->command({"seek", QString::number(s, 'f', 2), relative ? "relative" : "absolute+exact"});
}
void VideoView::addVolume(double d) { mpv_->command({"add", "volume", QString::number(d)}); }
void VideoView::toggleMute() { mpv_->command({"cycle", "mute"}); }

void VideoView::refreshControls() {
    play_->setIcon(QIcon::fromTheme(mpv_->paused ? "media-playback-start" : "media-playback-pause"));
    mute_->setIcon(QIcon::fromTheme(mpv_->muted ? "audio-volume-muted" : "audio-volume-high"));
    if (!seeking_) {
        QSignalBlocker b(seek_);
        seek_->setRange(0, int(mpv_->duration * 10));
        seek_->setValue(int(mpv_->position * 10));
    }
    { QSignalBlocker b(vol_); vol_->setValue(int(mpv_->volume)); }
    time_->setText(fmtTime(mpv_->position) + " / " + fmtTime(mpv_->duration));
}

QString VideoView::infoText() const {
    QString w = mpv_->propString("width"), h = mpv_->propString("height");
    QString vc = mpv_->propString("video-codec"), ac = mpv_->propString("audio-codec-name");
    QStringList parts;
    if (!w.isEmpty() && !h.isEmpty()) parts << w + " x " + h;
    if (!vc.isEmpty()) parts << vc.section(' ', 0, 0);
    if (!ac.isEmpty()) parts << ac;
    return parts.join("   ");
}

QString VideoView::timeText() const {
    return fmtTime(mpv_->position) + " / " + fmtTime(mpv_->duration) +
           QString("   vol %1%").arg(int(mpv_->volume)) + (mpv_->muted ? " (muted)" : "") + (mpv_->paused ? "   paused" : "");
}

// mpv-style keys
void VideoView::keyPressEvent(QKeyEvent* e) {
    bool shift = e->modifiers() & Qt::ShiftModifier;
    switch (e->key()) {
    case Qt::Key_Space: case Qt::Key_K: case Qt::Key_P: togglePause(); break;
    case Qt::Key_Left: seek(shift ? -1 : -5, true); break;
    case Qt::Key_Right: seek(shift ? 1 : 5, true); break;
    case Qt::Key_Up: seek(60, true); break;
    case Qt::Key_Down: seek(-60, true); break;
    case Qt::Key_9: case Qt::Key_Slash: addVolume(-5); break;
    case Qt::Key_0: case Qt::Key_Asterisk: addVolume(5); break;
    case Qt::Key_M: toggleMute(); break;
    case Qt::Key_Period: mpv_->command({"frame-step"}); break;
    case Qt::Key_Comma: mpv_->command({"frame-back-step"}); break;
    case Qt::Key_BracketLeft: mpv_->command({"multiply", "speed", "0.9091"}); break;
    case Qt::Key_BracketRight: mpv_->command({"multiply", "speed", "1.1"}); break;
    case Qt::Key_Backspace: mpv_->setProp("speed", "1"); break;
    case Qt::Key_L: mpv_->command({"ab-loop"}); break;
    case Qt::Key_F: case Qt::Key_Return: case Qt::Key_Enter: emit fullscreenRequested(); break;
    default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}
