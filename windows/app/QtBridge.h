// 核心库类型与 Qt 类型之间的转换。
#pragma once
#include "compositor/image.h"
#include <QImage>
#include <QString>
#include <filesystem>

// 不复制像素：QImage 直接引用 comp::Image 的缓冲区（调用方保证生命周期）。
QImage wrapImage(const comp::Image& image);
// 复制一份。
QImage toQImage(const comp::Image& image);
comp::Image fromQImage(const QImage& image);
QImage toQImage(const comp::GrayImage& image);

std::filesystem::path toPath(const QString& text);
QString fromPath(const std::filesystem::path& path);
