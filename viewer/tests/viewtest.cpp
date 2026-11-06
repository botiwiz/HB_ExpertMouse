// hb_viewtest <image> <out.png> [ops...]   ops: + - r l h v t fit 100 w   (renders offscreen)
#include "imageview.h"
#include <QApplication>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (qgetenv("DISPLAY").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (argc < 3) return 2;
    LoadCtl ctl;
    auto r = load_image(argv[1], 2000000000ull, ctl);
    if (!r->img) { fprintf(stderr, "%s\n", r->error.c_str()); return 1; }
    ImageView v;
    v.resize(1000, 700);
    v.show();
    v.setResult(std::shared_ptr<LoadResult>(std::move(r)), argv[1], false);
    for (int i = 3; i < argc; i++) {
        const char* o = argv[i];
        if (!strcmp(o, "+")) v.zoomStep(1); else if (!strcmp(o, "-")) v.zoomStep(-1);
        else if (!strcmp(o, "r")) v.rotate(1); else if (!strcmp(o, "l")) v.rotate(-1);
        else if (!strcmp(o, "h")) v.flipH(); else if (!strcmp(o, "v")) v.flipV();
        else if (!strcmp(o, "t")) v.tiled = !v.tiled; else if (!strcmp(o, "fit")) v.fit(true);
        else if (!strcmp(o, "100")) v.actualPixels(); else if (!strcmp(o, "w")) v.fitWidth();
    }
    v.grab().save(argv[2]);
    fprintf(stderr, "zoom %.1f%%\n", v.zoomPercent());
    return 0;
}
