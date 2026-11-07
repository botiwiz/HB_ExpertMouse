#include "thumb3d.h"
#ifdef HBV_HAVE_F3D
#include <f3d/engine.h>
#include <f3d/image.h>
#include <f3d/options.h>
#include <f3d/scene.h>
#include <f3d/window.h>
#endif
#include <QCoreApplication>
#include <QPointer>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
struct Job {
    QString path;
    int size = 512;
    QPointer<QObject> context;
    std::function<void(const QImage&, const QString&)> done;
};

std::mutex mu;
std::condition_variable cv;
std::unique_ptr<Job> pending;          // latest request only
bool started = false;

#ifdef HBV_HAVE_F3D
void worker() {
    std::unique_ptr<f3d::engine> eng;
    QString initError;
    for (;;) {
        std::unique_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [] { return pending != nullptr; });
            job = std::move(pending);
        }
        QImage img;
        QString err;
        if (!eng && initError.isEmpty()) {
            for (int k = 0; k < 2 && !eng; k++) {
                try { eng = std::make_unique<f3d::engine>(k == 0 ? f3d::engine::createEGL() : f3d::engine::createGLX(true)); }
                catch (const std::exception& e) { initError = QString::fromLocal8Bit(e.what()); }
            }
            if (eng) {
                initError.clear();
                f3d::options& o = eng->getOptions();
                auto set = [&o](const char* n, const char* v) { try { o.setAsString(n, v); } catch (...) {} };
                set("render.background.color", "0.11,0.11,0.12");
                set("render.grid.enable", "false");
                set("ui.axis", "false");
                set("ui.filename", "false");
                set("render.hdri.ambient", "true");
                set("render.effect.tone_mapping", "true");
                set("render.effect.antialiasing.mode", "fxaa");
            }
        }
        if (!eng) err = "3D preview unavailable: " + initError;
        else {
            try {
                eng->getWindow().setSize(job->size, job->size);
                f3d::scene& sc = eng->getScene();
                sc.clear();
                sc.add(std::filesystem::path(job->path.toStdString()));
                eng->getWindow().getCamera().resetToBounds();
                f3d::image fi = eng->getWindow().renderToImage();
                int w = (int)fi.getWidth(), h = (int)fi.getHeight(), ch = (int)fi.getChannelCount();
                if (w > 0 && h > 0 && (ch == 3 || ch == 4) && fi.getChannelType() == f3d::image::ChannelType::BYTE) {
                    QImage q(w, h, ch == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
                    const uint8_t* src = static_cast<const uint8_t*>(fi.getContent());
                    size_t row = (size_t)w * ch;
                    for (int y = 0; y < h; y++) memcpy(q.scanLine(y), src + (size_t)(h - 1 - y) * row, row);   // bottom-up
                    img = q;
                }
                sc.clear();                                   // free the model's memory between previews
            } catch (const std::exception& e) {
                err = QString::fromLocal8Bit(e.what());
            }
        }
        QPointer<QObject> ctx = job->context;
        auto done = job->done;
        if (ctx) QMetaObject::invokeMethod(ctx, [ctx, done, img, err] { if (ctx) done(img, err); }, Qt::QueuedConnection);
    }
}
#endif
} // namespace

bool Thumb3D::available() {
#ifdef HBV_HAVE_F3D
    return true;
#else
    return false;
#endif
}

bool Thumb3D::request(const QString& path, int size, QObject* context, std::function<void(const QImage&, const QString&)> done) {
#ifdef HBV_HAVE_F3D
    std::lock_guard<std::mutex> lk(mu);
    if (!started) {
        try { f3d::engine::autoloadPlugins(); } catch (...) {}
        QString app = QCoreApplication::applicationDirPath();       // bundled plugins (assimp, occt, usd...)
        std::vector<std::filesystem::path> dirs = {(app + "/../lib").toStdString(), (app + "/../lib/f3d").toStdString(), app.toStdString()};
        for (const char* p : {"assimp", "occt", "alembic", "draco", "usd", "hdf", "vdb"}) {
            try { f3d::engine::loadPlugin(p, dirs); } catch (...) {}
        }
        std::thread(worker).detach();
        started = true;
    }
    pending = std::make_unique<Job>(Job{path, size, context, std::move(done)});
    cv.notify_one();
    return true;
#else
    (void)path; (void)size; (void)context; (void)done;
    return false;
#endif
}
