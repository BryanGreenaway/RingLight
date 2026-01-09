/*
 * RingLight Overlay - Screen ring light for KDE Plasma 6
 * 
 * Creates white borders around a specific screen using layer-shell.
 * Click anywhere on the ring to quit.
 * License: MIT
 */

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QRasterWindow>
#include <QPainter>
#include <QMouseEvent>
#include <QCommandLineParser>
#include <QTimer>
#include <LayerShellQt/Window>
#include <LayerShellQt/Shell>

class RingPanel : public QRasterWindow {
public:
    enum Edge { Top, Bottom, Left, Right };
    
    RingPanel(Edge edge, int thickness, QColor color, QScreen *targetScreen)
        : m_color(color), m_edge(edge), m_thickness(thickness), m_targetScreen(targetScreen)
    {
        setFlag(Qt::FramelessWindowHint);
        
        // Set screen association FIRST
        setScreen(targetScreen);
        
        // Get screen dimensions
        QSize sz = targetScreen->size();
        
        // Set size based on edge
        if (edge == Top || edge == Bottom) {
            resize(sz.width(), thickness);
        } else {
            resize(thickness, sz.height());
        }
    }
    
    void setupLayerShell() {
        auto *layer = LayerShellQt::Window::get(this);
        if (!layer) {
            qWarning("Failed to get layer shell window");
            return;
        }
        
        // Configure layer shell
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setExclusiveZone(-1);  // Ignore panel reserved space
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        
        // Use unique scope per screen to prevent grouping
        QString scope = QString("ringlight-%1-%2").arg(m_targetScreen->name()).arg(int(m_edge));
        layer->setScope(scope);
        
        // Set anchors
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
    
    Edge edge() const { return m_edge; }
    
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
    int m_thickness;
    QScreen *m_targetScreen;
};

int main(int argc, char *argv[]) {
    // MUST be called before QGuiApplication
    LayerShellQt::Shell::useLayerShell();
    
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ringlight-overlay"));
    app.setApplicationVersion(QStringLiteral("1.0"));
    app.setQuitOnLastWindowClosed(true);
    
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Ring light overlay. Click to quit."));
    parser.addHelpOption();
    parser.addVersionOption();
    
    parser.addOption({{QStringLiteral("w"), QStringLiteral("width")},
        QStringLiteral("Border width"), QStringLiteral("px"), QStringLiteral("80")});
    parser.addOption({{QStringLiteral("c"), QStringLiteral("color")},
        QStringLiteral("Color as RRGGBB hex"), QStringLiteral("hex"), QStringLiteral("FFFFFF")});
    parser.addOption({{QStringLiteral("b"), QStringLiteral("brightness")},
        QStringLiteral("Brightness 1-100"), QStringLiteral("pct"), QStringLiteral("100")});
    parser.addOption({{QStringLiteral("s"), QStringLiteral("screen")},
        QStringLiteral("Screen index (0, 1, 2...) or name"), QStringLiteral("scr"), QString()});
    parser.addOption({{QStringLiteral("l"), QStringLiteral("list")},
        QStringLiteral("List screens")});
    
    parser.process(app);
    
    const auto screens = QGuiApplication::screens();
    
    if (parser.isSet(QStringLiteral("list"))) {
        for (int i = 0; i < screens.size(); ++i) {
            QScreen *s = screens[i];
            QRect geo = s->geometry();
            fprintf(stdout, "%d: %s (%dx%d @ %d,%d)\n", i, qPrintable(s->name()),
                    geo.width(), geo.height(), geo.x(), geo.y());
        }
        return 0;
    }
    
    // Find target screen
    QString screenArg = parser.value(QStringLiteral("screen"));
    QScreen *targetScreen = nullptr;
    int screenIdx = -1;
    
    if (screenArg.isEmpty()) {
        targetScreen = QGuiApplication::primaryScreen();
        screenIdx = screens.indexOf(targetScreen);
    } else {
        // Try as index first
        bool ok;
        int idx = screenArg.toInt(&ok);
        if (ok && idx >= 0 && idx < screens.size()) {
            targetScreen = screens[idx];
            screenIdx = idx;
        } else {
            // Try as name
            for (int i = 0; i < screens.size(); i++) {
                if (screens[i]->name() == screenArg) {
                    targetScreen = screens[i];
                    screenIdx = i;
                    break;
                }
            }
        }
    }
    
    if (!targetScreen) {
        fprintf(stderr, "Screen not found: %s\n", qPrintable(screenArg));
        fprintf(stderr, "Use -l to list available screens.\n");
        return 1;
    }
    
    // Parse settings
    int thickness = qBound(1, parser.value(QStringLiteral("width")).toInt(), 500);
    int brightness = qBound(1, parser.value(QStringLiteral("brightness")).toInt(), 100);
    
    QColor baseColor(QStringLiteral("#") + parser.value(QStringLiteral("color")));
    if (!baseColor.isValid()) baseColor = Qt::white;
    
    QColor color(
        baseColor.red() * brightness / 100,
        baseColor.green() * brightness / 100,
        baseColor.blue() * brightness / 100
    );
    
    QRect geo = targetScreen->geometry();
    fprintf(stdout, "Creating ring on screen %d: %s (%dx%d @ %d,%d)\n",
            screenIdx,
            qPrintable(targetScreen->name()),
            geo.width(), geo.height(), geo.x(), geo.y());
    
    // Create all 4 panels - pass target screen to constructor
    RingPanel *top = new RingPanel(RingPanel::Top, thickness, color, targetScreen);
    RingPanel *bottom = new RingPanel(RingPanel::Bottom, thickness, color, targetScreen);
    RingPanel *left = new RingPanel(RingPanel::Left, thickness, color, targetScreen);
    RingPanel *right = new RingPanel(RingPanel::Right, thickness, color, targetScreen);
    
    QList<RingPanel*> panels = {top, bottom, left, right};
    
    // Setup layer shell BEFORE showing
    for (RingPanel *p : panels) {
        p->setupLayerShell();
    }
    
    // Show all panels
    for (RingPanel *p : panels) {
        p->show();
    }
    
    fprintf(stdout, "Ring light active. Click on the ring to quit.\n");
    
    return app.exec();
}

