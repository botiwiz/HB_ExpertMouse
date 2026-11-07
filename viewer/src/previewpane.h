// Preview of one file: an ImageView canvas (same zoom / pan / rotate as the viewer) + file info.
// Pictures use HB Viewer's own decoder (quick preview first, then full resolution so zooming in
// stays sharp); 3D models are rendered by Thumb3D; videos/PDFs/documents use KIO thumbnailers;
// the type icon otherwise. Used by the cascade's last column and by the right-hand preview panel.
#pragma once
#include <KFileItem>
#include <QWidget>
#include <memory>

class QLabel;
class QImage;
class ImageView;
class ModelView;
class QStackedWidget;
struct LoadCtl;

class PreviewPane : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPane(QWidget* parent = nullptr);
    ~PreviewPane() override;
    void showItem(const KFileItem& item);
    void clearItem();
    KFileItem item() const { return item_; }
    ImageView* view() const { return view_; }
    ModelView* modelView() const { return model_; }   // live f3d view (nullptr until first used)
    bool live3D() const { return live_; }
    void goLive3D(QEvent* forward = nullptr);         // swap the 3D still for an orbitable f3d view

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void setPicture(const QImage& img);            // icon / thumbnail / 3D render
    void cancelLoad();
    void leaveLive3D();

    QStackedWidget* stack_;
    ImageView* view_;
    ModelView* model_ = nullptr;
    bool is3d_ = false, live_ = false;
    QString path3d_;
    QLabel* info_;
    QWidget* tools_;
    KFileItem item_;
    QString baseInfo_;
    int gen_ = 0;
    std::shared_ptr<LoadCtl> ctl_;                 // running full-resolution decode
};
