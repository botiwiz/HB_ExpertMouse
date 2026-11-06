// The image canvas: tiled + mip-mapped drawing, zoom/pan/rotate/flip, checkerboard,
// tiled (seamless) mode, pixel readout and animation playback.
#pragma once
#include "decode.h"
#include <QPointer>
#include <QWidget>
#include <memory>

class QMovie;

class ImageView : public QWidget {
    Q_OBJECT
public:
    enum Bg { BgDark, BgGray, BgWhite, BgChecker, BgCount };

    explicit ImageView(QWidget* parent = nullptr);
    ~ImageView() override;

    // show a decoded result; keepView = same file, higher-res version of what's shown
    void setResult(std::shared_ptr<LoadResult> r, const QString& path, bool keepView);
    void setMessage(const QString& msg);           // shown when there is no image
    void clear();

    Image* image() const { return r_ && r_->img ? r_->img.get() : nullptr; }
    std::shared_ptr<LoadResult> result() const { return r_; }
    QString path() const { return path_; }

    void fit(bool always);
    void zoomStep(int dir);
    void zoomStep(int dir, QPointF at);
    void actualPixels();
    void fitWidth();
    void rotate(int quarterTurns);
    void flipH();
    void flipV();
    void resetTransform();
    QImage transformedImage() const;               // full-res copy incl. rotation/flip

    double zoomPercent() const;                    // relative to the original file's pixels
    QString pixelText() const { return pixel_; }

    // options
    int bg = BgDark;
    bool smooth = true, tiled = false, wheelZoom = true, fitOnOpen = true;

signals:
    void navigate(int delta);                      // wheel / mouse back-forward buttons
    void contextMenuRequested(QPoint globalPos);
    void statusChanged();
    void doubleClicked();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    QTransform viewTransform() const;              // level-0 image px -> widget px
    void drawImageOnce(QPainter& p, double dx, double dy);
    void setZoom(double z, QPointF at);
    void updatePixel(QPointF pos);
    void rotatedSize(double& w, double& h) const;
    void startMovie();

    std::shared_ptr<LoadResult> r_;
    QString path_, message_;
    double zoom_ = 1, offx_ = 0, offy_ = 0;
    int rot_ = 0;
    bool flipH_ = false, flipV_ = false, autoFit_ = true;
    bool dragging_ = false;
    QPointF dragPos_;
    QString pixel_;
    QMovie* movie_ = nullptr;
};
