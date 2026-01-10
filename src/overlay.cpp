/*
 * RingLight Overlay - Screen ring light for KDE Plasma 6
 * 
 * Each process handles ONE screen using layer-shell.
 * For multi-monitor, spawn multiple processes with different -s values.
 * License: MIT
 */

#include <QGuiApplication>
#include <QScreen>
#include <QRasterWindow>
#include <QPainter>
#include <QMouseEvent>
#include <QCommandLineParser>
#include <LayerShellQt/Window>
#include <LayerShellQt/Shell>

class RingPanel : public QRasterWindow {
public:
    enum Edge { Top, Bottom, Left, Right };
    
    RingPanel(Edge edge, int thickness, QColor color, QScreen *screen)
        : m_color(color), m_edge(edge), m_screen(screen)
    {
        setFlag(Qt::FramelessWindowHint);
        setScreen(screen);
        
        // Size based on edge (layer-shell handles positioning via anchors)
        QSize sz = screen->size();
        if (edge == Top || edge == Bottom) {
            resize(sz.width(), thickness);
        } else {
            resize(thickness, sz.height());
        }
    }
    
    void setupLayerShell() {
        auto *layer = LayerShellQt::Window::get(this);
        if (!layer) return;
        
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layer->setScope(QStringLiteral("ringlight-%1-%2").arg(m_screen->name()).arg(int(m_edge)));
        
        LayerShellQt::Window::Anchors anchors;
        if (m_edge == Top || m_edge == Bottom) {
            anchors.setFlag(m_edge == Top ? LayerShellQt::Window::AnchorTop : LayerShellQt::Window::AnchorBottom);
            anchors.setFlag(LayerShellQt::Window::AnchorLeft);
            anchors.setFlag(LayerShellQt::Window::AnchorRight);
        } else {
            anchors.setFlag(m_edge == Left ? LayerShellQt::Window::AnchorLeft : LayerShellQt::Window::AnchorRight);
            anchors.setFlag(LayerShellQt::Window::AnchorTop);
            anchors.setFlag(LayerShellQt::Window::AnchorBottom);
        }
        layer->setAnchors(anchors);
    }
    
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter(this).fillRect(0, 0, width(), height(), m_color);
    }
    
    void mousePressEvent(QMouseEvent *) override { qApp->quit(); }
    
private:
    QColor m_color;
    Edge m_edge;
    QScreen *m_screen;
};

int main(int argc, char *argv[]) {
    // Always use layer-shell
    LayerShellQt::Shell::useLayerShell();
    
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ringlight-overlay"));
    app.setQuitOnLastWindowClosed(true);
    
    QCommandLineParser p;
    p.addHelpOption();
    p.addOption({{QStringLiteral("w"), QStringLiteral("width")}, QStringLiteral("Border width"), QStringLiteral("px"), QStringLiteral("80")});
    p.addOption({{QStringLiteral("c"), QStringLiteral("color")}, QStringLiteral("Color RRGGBB"), QStringLiteral("hex"), QStringLiteral("FFFFFF")});
    p.addOption({{QStringLiteral("b"), QStringLiteral("brightness")}, QStringLiteral("Brightness 1-100"), QStringLiteral("pct"), QStringLiteral("100")});
    p.addOption({{QStringLiteral("s"), QStringLiteral("screen")}, QStringLiteral("Screen index/name (one per process)"), QStringLiteral("scr")});
    p.addOption({{QStringLiteral("l"), QStringLiteral("list")}, QStringLiteral("List screens")});
    p.process(app);
    
    const auto screens = QGuiApplication::screens();
    
    if (p.isSet(QStringLiteral("list"))) {
        for (int i = 0; i < screens.size(); ++i) {
            QRect g = screens[i]->geometry();
            printf("%d: %s (%dx%d @ %d,%d)\n", i, qPrintable(screens[i]->name()), g.width(), g.height(), g.x(), g.y());
        }
        return 0;
    }
    
    int thickness = qBound(1, p.value(QStringLiteral("width")).toInt(), 500);
    int brightness = qBound(1, p.value(QStringLiteral("brightness")).toInt(), 100);
    
    QColor baseColor(QStringLiteral("#") + p.value(QStringLiteral("color")));
    if (!baseColor.isValid()) baseColor = Qt::white;
    QColor color(baseColor.red() * brightness / 100, baseColor.green() * brightness / 100, baseColor.blue() * brightness / 100);
    
    // Get target screen (only first -s is used, default to screen 0)
    QScreen *target = screens.isEmpty() ? nullptr : screens[0];
    QStringList screenArgs = p.values(QStringLiteral("screen"));
    if (!screenArgs.isEmpty()) {
        QString arg = screenArgs.first();
        bool ok;
        int idx = arg.toInt(&ok);
        if (ok && idx >= 0 && idx < screens.size()) {
            target = screens[idx];
        } else {
            for (QScreen *s : screens) {
                if (s->name() == arg) { target = s; break; }
            }
        }
    }
    
    if (!target) {
        fprintf(stderr, "No screen found\n");
        return 1;
    }
    
    printf("Ring on %s (%dx%d)\n", qPrintable(target->name()), target->size().width(), target->size().height());
    
    // Create 4 panels for this screen
    for (int e = 0; e < 4; e++) {
        auto *panel = new RingPanel(RingPanel::Edge(e), thickness, color, target);
        panel->setupLayerShell();
        panel->show();
    }
    
    return app.exec();
}
