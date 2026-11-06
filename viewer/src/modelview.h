// Embedded 3D model viewer: libf3d (https://f3d.app) renders OFFSCREEN into its own GL
// context; this widget only ever paints complete frames read back from it. Sharing Qt's GL
// surface (QOpenGLWidget) caused ghosting and stale frames, because f3d/VTK render from input
// events too, composite over the bound framebuffer and cache GL state.
// Mouse/keyboard go to f3d's own interactor, so all f3d controls work.
#pragma once
#include <QImage>
#include <QPoint>
#include <QWidget>
#include <memory>

namespace f3d { class engine; class interactor; }
class QTimer;

class ModelView : public QWidget {
    Q_OBJECT
public:
    explicit ModelView(QWidget* parent = nullptr);
    ~ModelView() override;

    void load(const QString& path);        // replaces the current model
    void unload();
    void command(const QString& cmd);      // f3d command, e.g. "reset_camera", "toggle render.grid.enable"
    QString path() const { return path_; }
    QString error() const { return error_; }
    bool engineOk() const { return engine_ != nullptr; }
    QImage frame() const { return frame_; }   // last complete frame (self-test)
    static QString f3dVersion();
    QString backend() const;                 // EGL / GLX / OSMesa (software)
    void setHalfRes(bool on);                // constant half-resolution rendering (+ FXAA)
    bool halfRes() const { return halfRes_; }

signals:
    void contextMenuRequested(QPoint globalPos);
    void loaded(bool ok);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;

private:
    bool ensureEngine();
    void requestFrame();                   // coalesced: at most one render per event-loop pass
    void renderFrame();
    void mods(Qt::KeyboardModifiers m);
    void syncSize();
    void busy();                           // keep refreshing for a moment (smooth camera, notifications)
    void setInteracting(bool on);          // half-resolution renders while dragging / scrolling
    double renderScale() const;            // window pixels per widget pixel
    void sendPos(QPointF widgetPos);

    std::unique_ptr<f3d::engine> engine_;
    f3d::interactor* inter_ = nullptr;
    QTimer *tick_ = nullptr, *frameTimer_ = nullptr;
    QImage frame_;
    QString path_, error_;
    QPoint pressPos_;
    bool halfRes_ = true;
    bool rightDown_ = false, animating_ = false, interacting_ = false, pendingMove_ = false;
    QSize windowSize_;                     // size last given to f3d's window (device px)
    QPointF lastPos_;                      // latest mouse position (widget px), sent once per frame
    Qt::KeyboardModifiers lastMods_;
    QTimer* settle_ = nullptr;             // ends wheel interaction
    qint64 busyUntil_ = 0;
};
