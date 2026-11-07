// HB Viewer - image viewer for huge PSD/PSB and common formats, with Dolphin (KDE) as its
// file browser. Pictures open in our viewer; any other file behaves exactly as in Dolphin.
#include "cli.h"
#include "columnbrowser.h"
#include "mainwindow.h"
#ifdef HBV_HAVE_F3D
#include "modelview.h"
#include "thumb3d.h"
#endif
#ifdef HBV_HAVE_MPV
#include "videoview.h"
#endif
#include <KColorSchemeManager>
#include <kcolorscheme_version.h>
#include <QAbstractItemModel>
#include <QFile>
#include <QImage>
#include <QOpenGLContext>
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QEventLoop>
#include <QKeyEvent>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <memory>

// Always dark: Breeze Dark colour scheme (also used by the embedded Dolphin view);
// a hand-made dark palette when the scheme isn't installed.
static void applyDark(QApplication& app) {
    if (QStyleFactory::keys().contains("Breeze", Qt::CaseInsensitive)) app.setStyle("Breeze");
    else app.setStyle(QStyleFactory::create("Fusion"));
#if KCOLORSCHEME_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    KColorSchemeManager* mgr = KColorSchemeManager::instance();
#else
    auto* mgr = new KColorSchemeManager(&app);
#endif
    QModelIndex idx;
    for (const char* name : {"Breeze Dark", "BreezeDark"}) {
        idx = mgr->indexForScheme(QString::fromLatin1(name));
        if (idx.isValid()) break;
    }
    if (idx.isValid()) { mgr->activateScheme(idx); return; }
    QPalette p;
    p.setColor(QPalette::Window, QColor(32, 35, 38));
    p.setColor(QPalette::WindowText, QColor(220, 220, 224));
    p.setColor(QPalette::Base, QColor(27, 30, 32));
    p.setColor(QPalette::AlternateBase, QColor(35, 38, 41));
    p.setColor(QPalette::Text, QColor(220, 220, 224));
    p.setColor(QPalette::Button, QColor(41, 44, 48));
    p.setColor(QPalette::ButtonText, QColor(220, 220, 224));
    p.setColor(QPalette::ToolTipBase, QColor(41, 44, 48));
    p.setColor(QPalette::ToolTipText, QColor(220, 220, 224));
    p.setColor(QPalette::Highlight, QColor(61, 174, 233));
    p.setColor(QPalette::HighlightedText, QColor(252, 252, 252));
    p.setColor(QPalette::Link, QColor(61, 174, 233));
    p.setColor(QPalette::PlaceholderText, QColor(130, 133, 138));
    for (auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
        p.setColor(QPalette::Disabled, role, QColor(110, 113, 118));
    app.setPalette(p);
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--convert")) {
        QCoreApplication core(argc, argv);
        return cli_convert(argc, argv);
    }
    QApplication app(argc, argv);
    app.setApplicationName("hb_viewer");
    app.setApplicationDisplayName("HB Viewer");
    app.setApplicationVersion("0.2.0");
    app.setDesktopFileName("hb_viewer");
    applyDark(app);

    auto* w = new MainWindow;
    w->show();

    // --selftest <folder> <screenshot.png>: load Dolphin, show the folder, report, save a screenshot
    if (argc > 3 && !strcmp(argv[1], "--selftest")) {
        w->resize(1200, 800);
        if (qEnvironmentVariableIsSet("HBV_SELFTEST_CASCADE")) w->setCascadeMode(true);
        w->openPath(QString::fromLocal8Bit(argv[2]));
        QString shot = QString::fromLocal8Bit(argv[3]);
        QTimer::singleShot(6000, w, [w, shot] {
            printf("dolphinpart: %s %s\n", w->dolphinLoaded() ? "LOADED" : "MISSING", w->dolphinError().toLocal8Bit().constData());
            printf("commands: %d\n", w->commandCount());
            int rc = w->dolphinLoaded() ? 0 : 3;
            if (qEnvironmentVariableIsSet("HBV_SELFTEST_MODES")) w->selftestViewModes();
            if (QByteArray f = qgetenv("HBV_SELFTEST_OPEN"); !f.isEmpty()) {
                // one activation of a non-viewable file must launch its default app exactly once
                w->selftestActivate(QUrl::fromLocalFile(QString::fromLocal8Bit(f)), QStringLiteral("text/plain"));
                QEventLoop l; QTimer::singleShot(4000, &l, &QEventLoop::quit); l.exec();
                QFile log(QStringLiteral("/tmp/hbv_open.log"));
                int launched = log.open(QIODevice::ReadOnly) ? log.readAll().count('\n') : -1;
                printf("open requests handled for one activation: %d -> %s  (external launches seen: %d)\n",
                       w->openRequests, w->openRequests == 1 ? "OK" : "FAIL", launched);
                if (w->openRequests != 1) rc = 8;
            }
            if (qEnvironmentVariableIsSet("HBV_SELFTEST_CASCADE")) {
                ColumnBrowser* c = w->cascade();
                int cols = c->columnCount();
                printf("cascade: columns=%d folder=%s selected=%lld\n", cols, c->url().toDisplayString().toLocal8Bit().constData(),
                       (long long)c->selectedItems().size());
                if (cols < 2) rc = 5;
                // keyboard, as real key presses into whatever widget has focus
                auto wait = [](int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); };
                w->activateWindow();
                c->view()->setFocus();
                auto press = [&](int key) {
                    QWidget* f = QApplication::focusWidget() ? QApplication::focusWidget() : c->view();
                    QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
                    QApplication::sendEvent(f, &ev);
                    wait(key == Qt::Key_Right ? 2000 : 300);
                };
                auto dir = [](const QUrl& u) { return u.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash); };
                QUrl start = c->currentUrl();
                press(Qt::Key_Right); QUrl in = c->currentUrl(); int inRow = c->currentRow();
                press(Qt::Key_Down);  QUrl in2 = c->currentUrl(); int in2Row = c->currentRow();
                press(Qt::Key_Left);  QUrl back = c->currentUrl();
                // one more vertical step in the parent column: Up if possible (start may be the last row)
                int backRow = c->currentRow(), rows = c->siblingCount();
                press(backRow > 0 ? Qt::Key_Up : Qt::Key_Down);
                QUrl sib = c->currentUrl();
                bool sibOk = rows < 2 ? sib == start : (dir(sib) == dir(start) && sib != start &&
                             c->currentRow() == (backRow > 0 ? backRow - 1 : backRow + 1));
                bool ok = dir(in) == start.adjusted(QUrl::StripTrailingSlash) && inRow == 0 &&
                          dir(in2) == dir(in) && in2Row == 1 && back == start && sibOk;
                printf("cascade keys: start=%s right=%s(row %d) down=%s(row %d) left=%s up/down=%s -> %s\n",
                       start.path().toLocal8Bit().constData(), in.path().toLocal8Bit().constData(), inRow,
                       in2.path().toLocal8Bit().constData(), in2Row, back.path().toLocal8Bit().constData(),
                       sib.path().toLocal8Bit().constData(), ok ? "OK" : "FAIL");
                if (!ok) rc = 6;
            }
#ifdef HBV_HAVE_F3D
            printf("f3d: %s\n", ModelView::f3dVersion().toLocal8Bit().constData());
            if (QByteArray f = qgetenv("HBV_SELFTEST_PREVIEW3D"); !f.isEmpty()) {
                // preview panel renderer: background thread, own offscreen engine
                struct R { QImage img; QString err; bool done = false; };
                auto r = std::make_shared<R>();
                Thumb3D::request(QString::fromLocal8Bit(f), 256, w, [r](const QImage& img, const QString& e) { r->img = img; r->err = e; r->done = true; });
                for (int i = 0; i < 200 && !r->done; i++) { QEventLoop l; QTimer::singleShot(100, &l, &QEventLoop::quit); l.exec(); }
                QImage got = r->img; QString err = r->err; bool doneFlag = r->done;
                long diff = 0;
                if (!got.isNull()) { QRgb bg = got.pixel(1, 1); for (int y = 0; y < got.height(); y++) for (int x = 0; x < got.width(); x++) if (got.pixel(x, y) != bg) diff++; }
                printf("preview3d: done=%d size=%dx%d drawn=%ld err=%s -> %s\n", doneFlag, got.width(), got.height(), diff,
                       err.toLocal8Bit().constData(), diff > 100 ? "OK" : "FAIL");
                if (diff <= 100) rc = 9;
            }
            if (ModelView* m = w->modelView(); m && !m->path().isEmpty()) {
                QImage fb = m->frame();
                // count pixels that differ from the background: >0 means the model was drawn
                QRgb bg = fb.isNull() ? 0 : fb.pixel(2, 2);
                long diff = 0;
                for (int y = 0; y < fb.height(); y += 2)
                    for (int x = 0; x < fb.width(); x += 2) if (fb.pixel(x, y) != bg) diff++;
                printf("f3d backend: %s\n", m->backend().toLocal8Bit().constData());
                printf("f3d model: %s engine=%d error='%s' size=%dx%d non_background_px=%ld\n",
                       m->path().toLocal8Bit().constData(), m->engineOk(), m->error().toLocal8Bit().constData(),
                       fb.width(), fb.height(), diff);
                fb.save(shot + ".3d.png");
                if (!m->engineOk() || !m->error().isEmpty() || diff < 100) rc = 4;
            }
#else
            printf("f3d: NOT BUILT\n");
#endif
#ifdef HBV_HAVE_MPV
            if (VideoView* v = w->videoView(); v && !v->path().isEmpty()) {
                // sample a few frames (a single grab right after a long idle wait can come back empty)
                QImage f;
                long lit = 0;
                for (int k = 0; k < 10 && lit < 100; k++) {
                    QEventLoop l; QTimer::singleShot(250, &l, &QEventLoop::quit); l.exec();
                    f = v->video()->grabFramebuffer();
                    lit = 0;
                    for (int y = 0; y < f.height(); y += 4) for (int x = 0; x < f.width(); x += 4) if (qGray(f.pixel(x, y)) > 30) lit++;
                }
                printf("video: %s  %s  lit_px=%ld  error='%s'  gl=%s\n", v->timeText().toLocal8Bit().constData(),
                       v->infoText().toLocal8Bit().constData(), lit, v->video()->error().toLocal8Bit().constData(),
                       v->video()->context() ? (v->video()->context()->isOpenGLES() ? "GLES" : "GL") : "none");
                f.save(shot + ".video.png");
                if (v->video()->position <= 0 || lit < 100) rc = 7;
            }
#else
            printf("video: NOT BUILT\n");
#endif
            w->grab().save(shot);
            fflush(stdout);
            qApp->exit(rc);
        });
        return app.exec();
    }

    w->openPath(argc > 1 ? QString::fromLocal8Bit(argv[1]) : w->lastFolder());
    return app.exec();
}
