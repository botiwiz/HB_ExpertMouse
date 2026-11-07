// 3D model previews for the preview panel: a background thread with its own offscreen f3d
// engine (GPU: EGL, else GLX) renders the requested model to an image. Only the most recent
// request is kept, so scrolling through a folder never queues up stale renders.
#pragma once
#include <QImage>
#include <QObject>
#include <QString>
#include <functional>

namespace Thumb3D {
// Render 'path' at size x size; 'done' runs on the GUI thread (only if 'context' still exists).
// Returns false when this build has no f3d.
bool request(const QString& path, int size, QObject* context, std::function<void(const QImage&, const QString& error)> done);
bool available();
}
