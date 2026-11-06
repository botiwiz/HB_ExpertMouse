// hb_videotest <url-or-file> <out.png> : play for 2 s offscreen, report state, grab a frame
#include "videoview.h"
#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QTimer>
#include <cstdio>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 3) return 2;
    printf("libmpv available: %d\n", VideoView::available());
    VideoView v;
    v.resize(800, 500);
    v.show();
    if (!v.ok()) { printf("error: %s\n", v.error().toLocal8Bit().constData()); return 1; }
    v.open(QString::fromLocal8Bit(argv[1]));
    QEventLoop l; QTimer::singleShot(2500, &l, &QEventLoop::quit); l.exec();
    QImage f = v.video()->grabFramebuffer();
    long lit = 0;
    for (int y = 0; y < f.height(); y += 4) for (int x = 0; x < f.width(); x += 4) if (qGray(f.pixel(x, y)) > 30) lit++;
    printf("time: %s\ninfo: %s\nframe %dx%d lit_px=%ld\n", v.timeText().toLocal8Bit().constData(),
           v.infoText().toLocal8Bit().constData(), f.width(), f.height(), lit);
    f.save(argv[2]);
    v.togglePause();
    QEventLoop l2; QTimer::singleShot(300, &l2, &QEventLoop::quit); l2.exec();
    printf("after pause: %s\n", v.timeText().toLocal8Bit().constData());
    return lit > 100 ? 0 : 4;
}
