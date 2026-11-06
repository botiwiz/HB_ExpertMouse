// HB Viewer - image viewer for huge PSD/PSB and common formats, with Dolphin (KDE) as its
// file browser. Pictures open in our viewer; any other file behaves exactly as in Dolphin.
#include "cli.h"
#include "columnbrowser.h"
#include "mainwindow.h"
#ifdef HBV_HAVE_F3D
#include "modelview.h"
#endif
#include <KColorSchemeManager>
#include <kcolorscheme_version.h>
#include <QAbstractItemModel>
#include <QImage>
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>
#include <cstdio>
#include <cstring>

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
            if (qEnvironmentVariableIsSet("HBV_SELFTEST_CASCADE")) {
                ColumnBrowser* c = w->cascade();
                int cols = c->columnCount();
                printf("cascade: columns=%d folder=%s selected=%lld\n", cols, c->url().toDisplayString().toLocal8Bit().constData(),
                       (long long)c->selectedItems().size());
                if (cols < 2) rc = 5;
            }
#ifdef HBV_HAVE_F3D
            printf("f3d: %s\n", ModelView::f3dVersion().toLocal8Bit().constData());
            if (ModelView* m = w->modelView(); m && !m->path().isEmpty()) {
                QImage fb = m->grabFramebuffer();
                // count pixels that differ from the background: >0 means the model was drawn
                QRgb bg = fb.isNull() ? 0 : fb.pixel(2, 2);
                long diff = 0;
                for (int y = 0; y < fb.height(); y += 2)
                    for (int x = 0; x < fb.width(); x += 2) if (fb.pixel(x, y) != bg) diff++;
                printf("f3d model: %s engine=%d error='%s' size=%dx%d non_background_px=%ld\n",
                       m->path().toLocal8Bit().constData(), m->engineOk(), m->error().toLocal8Bit().constData(),
                       fb.width(), fb.height(), diff);
                fb.save(shot + ".3d.png");
                if (!m->engineOk() || !m->error().isEmpty() || diff < 100) rc = 4;
            }
#else
            printf("f3d: NOT BUILT\n");
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
