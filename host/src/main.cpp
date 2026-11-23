#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>

#include "ui/mainwindow.h"

namespace {

// 固定浅色主题，避免 Windows 深色模式把控件文字变成白色而背景仍是浅色。
// Pin a light palette so Windows dark mode cannot turn widget text white
// while our stylesheet keeps the backgrounds light (white-on-white).
void applyLightTheme()
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window,          QColor("#F5F7FA"));
    p.setColor(QPalette::WindowText,      QColor("#1F2937"));
    p.setColor(QPalette::Base,            QColor("#FFFFFF"));
    p.setColor(QPalette::AlternateBase,   QColor("#F1F5F9"));
    p.setColor(QPalette::Text,            QColor("#1F2937"));
    p.setColor(QPalette::Button,          QColor("#FFFFFF"));
    p.setColor(QPalette::ButtonText,      QColor("#1F2937"));
    p.setColor(QPalette::BrightText,      QColor("#FFFFFF"));
    p.setColor(QPalette::Highlight,       QColor("#2563EB"));
    p.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    p.setColor(QPalette::ToolTipBase,     QColor("#FFFFFF"));
    p.setColor(QPalette::ToolTipText,     QColor("#1F2937"));
    p.setColor(QPalette::PlaceholderText, QColor("#94A3B8"));
    p.setColor(QPalette::Disabled, QPalette::Text,       QColor("#94A3B8"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#94A3B8"));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#94A3B8"));
    QApplication::setPalette(p);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("Timing Generator Host");
    QApplication::setOrganizationName("stm32-precision-timing-generator");
    applyLightTheme();

    MainWindow w;
    w.show();
    return app.exec();
}
