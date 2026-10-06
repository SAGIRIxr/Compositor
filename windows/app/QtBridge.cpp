#include "QtBridge.h"
#include <cstring>

QImage wrapImage(const comp::Image& image) {
    return QImage(image.pixels.data(), image.width, image.height, int(image.stride()), QImage::Format_RGBA8888_Premultiplied);
}

QImage toQImage(const comp::Image& image) {
    return wrapImage(image).copy();
}

comp::Image fromQImage(const QImage& source) {
    QImage converted = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    comp::Image image(converted.width(), converted.height());
    for (int y = 0; y < converted.height(); ++y) {
        std::memcpy(image.row(y), converted.constScanLine(y), image.stride());
    }
    return image;
}

QImage toQImage(const comp::GrayImage& image) {
    QImage out(image.width, image.height, QImage::Format_Grayscale8);
    for (int y = 0; y < image.height; ++y) std::memcpy(out.scanLine(y), image.row(y), size_t(image.width));
    return out;
}

std::filesystem::path toPath(const QString& text) {
#ifdef _WIN32
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::u8path(text.toStdString());
#endif
}

QString fromPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.u8string());
#endif
}
