/*
 * RingLight - Screen ring light for KDE Plasma 6
 * 
 * Uses layer-shell-qt for proper Wayland overlay support on Plasma.
 * Click anywhere on the ring to quit.
 *
 * License: MIT
 */

#include <QGuiApplication>
#include <QScreen>
#include <QRasterWindow>
#include <QPainter>
#include <QMouseEvent>
#include <QCommandLineParser>
#include <QDebug>
#include <LayerShellQt/Window>
#include <LayerShellQt/Shell>

class RingPanel : public QRasterWindow {
public:
    enum Edge { Top, Bottom, Left, Right };
    
    RingPanel(Edge edge, int borderWidth, QColor color)
        : m_color(color), m_edge(edge), m_borderWidth(borderWidth)
    {
        setFlag(Qt::FramelessWindowHint);
    }
    
    void initForScreen(QScreen *screen) {
        m_screen = screen;
        
        // Set screen association
        setScreen(screen);
        
        // Force window handle creation
        create();
        
        // Get screen geometry (includes position for multi-monitor)
        QRect screenGeo = screen->geometry();
        QSize screenSize = screen->size();
        
        // Calculate position and size based on edge
        QRect panelGeo;
        switch (m_edge) {
        case Top:
            panelGeo = QRect(screenGeo.x(), screenGeo.y(), 
                            screenSize.width(), m_borderWidth);
            break;
        case Bottom:
            panelGeo = QRect(screenGeo.x(), screenGeo.y() + screenSize.height() - m_borderWidth,
                            screenSize.width(), m_borderWidth);
            break;
        case Left:
            panelGeo = QRect(screenGeo.x(), screenGeo.y(),
                            m_borderWidth, screenSize.height());
            break;
        case Right:
            panelGeo = QRect(screenGeo.x() + screenSize.width() - m_borderWidth, screenGeo.y(),
                            m_borderWidth, screenSize.height());
            break;
        }
        
        setGeometry(panelGeo);
    }
    
    void setupLayerShell() {
        auto *layer = LayerShellQt::Window::get(this);
        if (!layer) {
            qWarning("Failed to get LayerShellQt::Window");
            return;
        }
        
        // Debug: show which screen we think we're on
        if (m_screen) {
            qDebug("Setting up layer shell for screen: %s", qPrintable(m_screen->name()));
        }
        
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setExclusiveZone(-1);
        layer->setScope(QStringLiteral("ringlight"));
        
        // Don't block keyboard but allow mouse clicks
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        
        LayerShellQt::Window::Anchors anchors;
        switch (m_edge) {
        case Top:
            anchors.setFlag(LayerShellQt::Window::AnchorTop);
            anchors.setFlag(LayerShellQt::Window::AnchorLeft);
            anchors.setFlag(LayerShellQt::Window::AnchorRight);
            break;
        case Bottom:
            anchors.setFlag(LayerShellQt::Window::AnchorBottom);
            anchors.setFlag(LayerShellQt::Window::AnchorLeft);
            anchors.setFlag(LayerShellQt::Window::AnchorRight);
            break;
        case Left:
            anchors.setFlag(LayerShellQt::Window::AnchorLeft);
            anchors.setFlag(LayerShellQt::Window::AnchorTop);
            anchors.setFlag(LayerShellQt::Window::AnchorBottom);
            break;
        case Right:
            anchors.setFlag(LayerShellQt::Window::AnchorRight);
            anchors.setFlag(LayerShellQt::Window::AnchorTop);
            anchors.setFlag(LayerShellQt::Window::AnchorBottom);
            break;
        }
        layer->setAnchors(anchors);
    }
    
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(QRect(0, 0, width(), height()), m_color);
    }
    
    void mousePressEvent(QMouseEvent *) override {
        qApp->quit();
    }
    
private:
    QColor m_color;
    Edge m_edge;
    int m_borderWidth;
    QScreen *m_screen = nullptr;
};

int main(int argc, char *argv[]) {
    // MUST be before QGuiApplication
    LayerShellQt::Shell::useLayerShell();
    
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ringlight"));
    app.setApplicationVersion(QStringLiteral("1.0"));
    
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Screen ring light for webcam illumination.\n"
        "Click on the ring to quit."));
    parser.addHelpOption();
    parser.addVersionOption();
    
    parser.addOption({{QStringLiteral("w"), QStringLiteral("width")},
        QStringLiteral("Border width in pixels"), QStringLiteral("px"), QStringLiteral("80")});
    parser.addOption({{QStringLiteral("b"), QStringLiteral("brightness")},
        QStringLiteral("Brightness 0-100"), QStringLiteral("pct"), QStringLiteral("100")});
    parser.addOption({{QStringLiteral("c"), QStringLiteral("color")},
        QStringLiteral("Color in hex RRGGBB"), QStringLiteral("hex"), QStringLiteral("FFFFFF")});
    parser.addOption({{QStringLiteral("s"), QStringLiteral("screen")},
        QStringLiteral("Screen name (from -l output)"), QStringLiteral("name"), QString()});
    parser.addOption({{QStringLiteral("l"), QStringLiteral("list")},
        QStringLiteral("List screens and exit")});
    
    parser.process(app);
    
    const auto screens = QGuiApplication::screens();
    
    if (parser.isSet(QStringLiteral("list"))) {
        fprintf(stdout, "Available screens:\n");
        for (int i = 0; i < screens.size(); ++i) {
            QScreen *s = screens[i];
            fprintf(stdout, "  %d: %s (%dx%d)\n", i, qPrintable(s->name()),
                    s->size().width(), s->size().height());
        }
        fprintf(stdout, "\nUsage:\n");
        fprintf(stdout, "  ringlight              # All screens\n");
        fprintf(stdout, "  ringlight -s DP-1      # Specific screen by name\n");
        fprintf(stdout, "  ringlight -s HDMI-A-1  # Another screen\n");
        return 0;
    }
    
    int borderWidth = qBound(1, parser.value(QStringLiteral("width")).toInt(), 500);
    int brightness = qBound(1, parser.value(QStringLiteral("brightness")).toInt(), 100);
    QString screenArg = parser.value(QStringLiteral("screen"));
    
    QColor baseColor(QStringLiteral("#") + parser.value(QStringLiteral("color")));
    if (!baseColor.isValid()) baseColor = Qt::white;
    
    QColor color(
        baseColor.red() * brightness / 100,
        baseColor.green() * brightness / 100,
        baseColor.blue() * brightness / 100
    );
    
    // Determine which screens to use
    QList<QScreen*> targetScreens;
    
    if (screenArg.isEmpty()) {
        // All screens
        targetScreens = screens;
    } else {
        // Find screen by name (exact or partial match)
        for (QScreen *s : screens) {
            if (s->name() == screenArg || 
                s->name().contains(screenArg, Qt::CaseInsensitive)) {
                targetScreens << s;
                break;
            }
        }
        
        // Also try as index
        if (targetScreens.isEmpty()) {
            bool ok;
            int idx = screenArg.toInt(&ok);
            if (ok && idx >= 0 && idx < screens.size()) {
                targetScreens << screens[idx];
            }
        }
        
        if (targetScreens.isEmpty()) {
            fprintf(stderr, "Screen not found: %s\n", qPrintable(screenArg));
            fprintf(stderr, "Run with -l to list available screens.\n");
            return 1;
        }
    }
    
    QList<RingPanel*> allPanels;
    
    for (QScreen *screen : targetScreens) {
        fprintf(stdout, "Creating ring on: %s (%dx%d)\n",
                qPrintable(screen->name()),
                screen->size().width(),
                screen->size().height());
        
        // Create 4 panels per screen
        auto *top = new RingPanel(RingPanel::Top, borderWidth, color);
        auto *bottom = new RingPanel(RingPanel::Bottom, borderWidth, color);
        auto *left = new RingPanel(RingPanel::Left, borderWidth, color);
        auto *right = new RingPanel(RingPanel::Right, borderWidth, color);
        
        // Initialize for this screen (sets screen and geometry)
        top->initForScreen(screen);
        bottom->initForScreen(screen);
        left->initForScreen(screen);
        right->initForScreen(screen);
        
        // Show first to create the Wayland surface
        top->show();
        bottom->show();
        left->show();
        right->show();
        
        // Then setup layer shell on the existing surface
        top->setupLayerShell();
        bottom->setupLayerShell();
        left->setupLayerShell();
        right->setupLayerShell();
        
        allPanels << top << bottom << left << right;
    }
    
    if (allPanels.isEmpty()) {
        fprintf(stderr, "No panels created.\n");
        return 1;
    }
    
    fprintf(stdout, "Ring light active on %d screen(s). Click ring to quit.\n", 
            targetScreens.size());
    
    return app.exec();
}
