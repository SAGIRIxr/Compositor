#include "MainWindow.h"
#include <QApplication>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>

int main(int argc, char* argv[]) {
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Compositor"));
    QApplication::setApplicationName(QStringLiteral("Compositor"));
    QApplication::setApplicationVersion(QStringLiteral(COMPOSITOR_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/compositor.png")));

    // 深色界面，与修图软件的习惯一致。
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(50, 50, 52));
    palette.setColor(QPalette::WindowText, QColor(225, 225, 225));
    palette.setColor(QPalette::Base, QColor(36, 36, 38));
    palette.setColor(QPalette::AlternateBase, QColor(56, 56, 58));
    palette.setColor(QPalette::ToolTipBase, QColor(60, 60, 62));
    palette.setColor(QPalette::ToolTipText, QColor(230, 230, 230));
    palette.setColor(QPalette::Text, QColor(225, 225, 225));
    palette.setColor(QPalette::Button, QColor(58, 58, 60));
    palette.setColor(QPalette::ButtonText, QColor(225, 225, 225));
    palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor(80, 160, 240));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 120));
    QApplication::setPalette(palette);

    MainWindow window;
    window.show();
    // 命令行或“打开方式”传进来的文件。
    const QStringList arguments = QApplication::arguments();
    for (int i = 1; i < arguments.size(); ++i) window.openPath(arguments[i]);
    return QApplication::exec();
}
