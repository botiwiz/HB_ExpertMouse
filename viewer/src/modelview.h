// Embedded 3D model viewer: libf3d (https://f3d.app) rendering into a QOpenGLWidget.
// Mouse/keyboard go to f3d's own interactor, so all f3d controls work (left = rotate,
// middle = pan, right/wheel = zoom, plus f3d's keys; F1 is ours, f3d's cheatsheet is a command).
#pragma once
#include <QOpenGLWidget>
#include <QPoint>
#include <memory>

namespace f3d { class engine; class interactor; }
class QTimer;

class ModelView : public QOpenGLWidget {
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
    static QString f3dVersion();

signals:
    void contextMenuRequested(QPoint globalPos);
    void loaded(bool ok);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;

private:
    void doLoad();
    void mods(Qt::KeyboardModifiers m);

    std::unique_ptr<f3d::engine> engine_;
    f3d::interactor* inter_ = nullptr;
    QTimer* tick_ = nullptr;
    QString path_, pending_, error_;
    QPoint pressPos_;
    bool rightDown_ = false;
};
