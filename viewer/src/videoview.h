// Embedded video/audio player built on libmpv (the engine of mpv, Haruna, Celluloid).
// libmpv is loaded at runtime (libmpv.so.2) so the app still starts without it; video frames
// are rendered through mpv's OpenGL render API into a QOpenGLWidget.
#pragma once
#include <QOpenGLWidget>
#include <QString>
#include <QWidget>

struct mpv_handle;
struct mpv_render_context;
class QLabel;
class QSlider;
class QToolButton;

class MpvWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit MpvWidget(QWidget* parent = nullptr);
    ~MpvWidget() override;
    bool ok() const { return mpv_ != nullptr; }
    QString error() const { return error_; }

    void command(const QStringList& args);
    void setProp(const char* name, const QString& value);
    QString propString(const char* name) const;

    double position = 0, duration = 0, volume = 100;
    int updatesRequested = 0, paints = 0, renders = 0;   // diagnostics
    bool softwareGL = false;              // no GPU: llvmpipe / softpipe / swrast
    QString renderer;                     // GL_RENDERER
    bool paused = false, muted = false;

signals:
    void changed();                       // position / duration / pause / volume / file info
    void fileLoaded();
    void clicked();
    void doubleClicked();
    void wheel(int steps);
    void menuRequested(QPoint globalPos);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    void drainEvents();
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* rctx_ = nullptr;
    QString error_;
    bool clickPending_ = false;
};

class VideoView : public QWidget {
    Q_OBJECT
public:
    explicit VideoView(QWidget* parent = nullptr);
    static bool available();              // libmpv found on this system
    bool ok() const { return mpv_ && mpv_->ok(); }
    QString error() const;

    void open(const QString& path);
    void stop();                          // stop playback (leaving the page)
    void togglePause();
    void seek(double seconds, bool relative);
    void addVolume(double delta);
    void toggleMute();
    QString infoText() const;             // for the status bar: resolution, codec
    QString timeText() const;             // "1:23 / 4:56   vol 80%"
    QString path() const { return path_; }
    MpvWidget* video() const { return mpv_; }

signals:
    void statusChanged();
    void contextMenuRequested(QPoint globalPos);
    void fullscreenRequested();

protected:
    void keyPressEvent(QKeyEvent*) override;

private:
    void refreshControls();
    MpvWidget* mpv_ = nullptr;
    QToolButton *play_ = nullptr, *mute_ = nullptr, *full_ = nullptr;
    QSlider *seek_ = nullptr, *vol_ = nullptr;
    QLabel* time_ = nullptr;
    QString path_;
    bool seeking_ = false;
};
