#include "cli.h"
#include "decode.h"
#include <QImage>
#include <QImageWriter>
#include <chrono>
#include <cstdio>
#include <cstdlib>

int cli_convert(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s --convert <input> <output.png|jpg|bmp|tiff|...> [max_megapixels]\n", argv[0]); return 2; }
    LoadCtl ctl;
    uint64_t budget = argc > 4 && atoll(argv[4]) > 0 ? (uint64_t)atoll(argv[4]) * 1000000 : 2000000000ull;
    auto t0 = std::chrono::steady_clock::now();
    auto r = load_image(argv[2], budget, ctl);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!r->img) { fprintf(stderr, "%s: %s\n", argv[2], r->error.c_str()); return 1; }
    fprintf(stderr, "%s: %s %s %dx%d (1/%d) %.3fs\n", argv[2], r->info.format.c_str(), r->info.details.c_str(),
            r->info.orig_w, r->info.orig_h, r->info.scale_div, secs);
    QImage q = qimage_from_image(r->img.get());
    if (q.isNull()) { fprintf(stderr, "image too large to save\n"); return 1; }
    QImageWriter w(QString::fromLocal8Bit(argv[3]));
    if (!w.write(q)) { fprintf(stderr, "%s\n", w.errorString().toLocal8Bit().constData()); return 1; }
    return 0;
}
