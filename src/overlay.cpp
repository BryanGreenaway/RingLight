/*
 * RingLight Overlay - Screen ring light for KDE Plasma 6
 * 
 * Uses wlr-layer-shell protocol directly for proper multi-monitor support.
 * Each process handles ONE screen.
 * License: MIT
 */

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QRasterWindow>
#include <QPainter>
#include <QMouseEvent>
#include <QCommandLineParser>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

// Qt Wayland private headers for native access
#include <private/qguiapplication_p.h>
#include <qpa/qplatformnativeinterface.h>

// Wayland - wrap C header to handle 'namespace' keyword conflict
#include <wayland-client.h>
extern "C" {
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client.h"
#undef namespace
}

// Config
struct Config {
    int width = 80;
    int brightness = 100;
    QString color = QStringLiteral("FFFFFF");
    bool fullscreen = false;
};

static Config loadConfig() {
    Config cfg;
    QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    QSettings s(path + "/ringlight/config.ini", QSettings::IniFormat);
    
    cfg.color = s.value("color", "#FFFFFF").toString();
    if (cfg.color.startsWith('#')) cfg.color = cfg.color.mid(1);
    cfg.brightness = qBound(1, s.value("brightness", 100).toInt(), 100);
    cfg.width = qBound(1, s.value("width", 80).toInt(), 500);
    cfg.fullscreen = s.value("fullscreen", false).toBool();
    
    return cfg;
}

// Global Wayland state
static struct wl_display *wl_display = nullptr;
static struct wl_registry *wl_registry = nullptr;
static struct zwlr_layer_shell_v1 *layer_shell = nullptr;

// Registry listener
static void registry_global(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = (zwlr_layer_shell_v1*)wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, 
            std::min(version, 4u));
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove
};

// Layer surface state
struct LayerSurfaceData {
    QWindow *window = nullptr;
    struct zwlr_layer_surface_v1 *layer_surface = nullptr;
    bool configured = false;
    uint32_t width = 0;
    uint32_t height = 0;
};

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                   uint32_t serial, uint32_t width, uint32_t height) {
    LayerSurfaceData *ls = (LayerSurfaceData*)data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    
    if (width > 0 && height > 0) {
        ls->width = width;
        ls->height = height;
        if (ls->window) {
            ls->window->resize(width, height);
        }
    }
    ls->configured = true;
    
    // Commit after configure
    if (ls->window) {
        auto *native = QGuiApplication::platformNativeInterface();
        auto *wl_surface = (struct wl_surface*)native->nativeResourceForWindow("surface", ls->window);
        if (wl_surface) {
            wl_surface_commit(wl_surface);
        }
    }
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    qApp->quit();
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    layer_surface_configure,
    layer_surface_closed
};

class OverlayPanel : public QRasterWindow {
public:
    enum Edge { Top, Bottom, Left, Right, Full };
    
    OverlayPanel(Edge edge, int thickness, QColor color, QScreen *screen)
        : m_color(color), m_edge(edge), m_screen(screen), m_thickness(thickness)
    {
        setFlag(Qt::FramelessWindowHint);
        setScreen(screen);
        
        // Calculate size based on edge
        QSize sz = screen->size();
        if (edge == Full) {
            resize(sz);
        } else if (edge == Top || edge == Bottom) {
            resize(sz.width(), thickness);
        } else {
            resize(thickness, sz.height());
        }
    }
    
    ~OverlayPanel() {
        if (m_layerData.layer_surface) {
            zwlr_layer_surface_v1_destroy(m_layerData.layer_surface);
        }
    }
    
    bool setupLayerShell() {
        if (!layer_shell) {
            fprintf(stderr, "Layer shell not available\n");
            return false;
        }
        
        auto *native = QGuiApplication::platformNativeInterface();
        
        // Get wl_surface from Qt window
        auto *wl_surface = (struct wl_surface*)native->nativeResourceForWindow("surface", this);
        if (!wl_surface) {
            fprintf(stderr, "Failed to get wl_surface\n");
            return false;
        }
        
        // Get wl_output for the target screen
        auto *wl_output = (struct wl_output*)native->nativeResourceForScreen("output", m_screen);
        if (!wl_output) {
            fprintf(stderr, "Warning: Failed to get wl_output for screen %s, using default\n", 
                    qPrintable(m_screen->name()));
        }
        
        // Create layer surface with the specific output
        m_layerData.window = this;
        m_layerData.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell,
            wl_surface,
            wl_output,  // THIS is the key - specifying which output!
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            "ringlight"
        );
        
        if (!m_layerData.layer_surface) {
            fprintf(stderr, "Failed to create layer surface\n");
            return false;
        }
        
        // Add listener
        zwlr_layer_surface_v1_add_listener(m_layerData.layer_surface, 
                                           &layer_surface_listener, &m_layerData);
        
        // Set size
        zwlr_layer_surface_v1_set_size(m_layerData.layer_surface, width(), height());
        
        // Set anchors based on edge
        uint32_t anchor = 0;
        switch (m_edge) {
        case Top:
            anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            break;
        case Bottom:
            anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            break;
        case Left:
            anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
            break;
        case Right:
            anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
            break;
        case Full:
            anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | 
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            break;
        }
        zwlr_layer_surface_v1_set_anchor(m_layerData.layer_surface, anchor);
        
        // Set exclusive zone to -1 (don't reserve space, extend under other surfaces)
        zwlr_layer_surface_v1_set_exclusive_zone(m_layerData.layer_surface, -1);
        
        // No keyboard focus
        zwlr_layer_surface_v1_set_keyboard_interactivity(m_layerData.layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        
        // Commit to apply
        wl_surface_commit(wl_surface);
        
        return true;
    }
    
    QScreen *targetScreen() const { return m_screen; }
    bool isConfigured() const { return m_layerData.configured; }
    
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter(this).fillRect(0, 0, width(), height(), m_color);
    }
    
    void mousePressEvent(QMouseEvent *) override { 
        qApp->quit(); 
    }
    
private:
    QColor m_color;
    Edge m_edge;
    QScreen *m_screen;
    int m_thickness;
    LayerSurfaceData m_layerData;
};

int main(int argc, char *argv[]) {
    // Force Wayland platform
    qputenv("QT_QPA_PLATFORM", "wayland");
    
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ringlight-overlay"));
    app.setQuitOnLastWindowClosed(true);
    
    // Load config
    Config cfg = loadConfig();
    
    QCommandLineParser p;
    p.addHelpOption();
    p.addOption({{QStringLiteral("w"), QStringLiteral("width")}, QStringLiteral("Border width (0 = from config)"), QStringLiteral("px"), QStringLiteral("0")});
    p.addOption({{QStringLiteral("c"), QStringLiteral("color")}, QStringLiteral("Color RRGGBB (empty = from config)"), QStringLiteral("hex"), QString()});
    p.addOption({{QStringLiteral("b"), QStringLiteral("brightness")}, QStringLiteral("Brightness 1-100 (0 = from config)"), QStringLiteral("pct"), QStringLiteral("0")});
    p.addOption({{QStringLiteral("s"), QStringLiteral("screen")}, QStringLiteral("Screen index/name"), QStringLiteral("scr")});
    p.addOption({{QStringLiteral("f"), QStringLiteral("fullscreen")}, QStringLiteral("Fullscreen mode (whole screen lights up)")});
    p.addOption({{QStringLiteral("l"), QStringLiteral("list")}, QStringLiteral("List screens")});
    p.process(app);
    
    const auto screens = QGuiApplication::screens();
    auto *native = QGuiApplication::platformNativeInterface();
    
    if (p.isSet(QStringLiteral("list"))) {
        for (int i = 0; i < screens.size(); ++i) {
            QRect g = screens[i]->geometry();
            auto *output = native->nativeResourceForScreen("output", screens[i]);
            printf("%d: %s (%dx%d @ %d,%d) [wl_output=%p]\n", 
                   i, qPrintable(screens[i]->name()), 
                   g.width(), g.height(), g.x(), g.y(),
                   output);
        }
        return 0;
    }
    
    // Apply CLI overrides
    int cliWidth = p.value(QStringLiteral("width")).toInt();
    if (cliWidth > 0) cfg.width = qBound(1, cliWidth, 500);
    
    int cliBright = p.value(QStringLiteral("brightness")).toInt();
    if (cliBright > 0) cfg.brightness = qBound(1, cliBright, 100);
    
    QString cliColor = p.value(QStringLiteral("color"));
    if (!cliColor.isEmpty()) cfg.color = cliColor;
    
    if (p.isSet(QStringLiteral("fullscreen"))) cfg.fullscreen = true;
    
    // Calculate final color
    QColor baseColor(QStringLiteral("#") + cfg.color);
    if (!baseColor.isValid()) baseColor = Qt::white;
    QColor color(baseColor.red() * cfg.brightness / 100, 
                 baseColor.green() * cfg.brightness / 100, 
                 baseColor.blue() * cfg.brightness / 100);
    
    // Get target screen
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
    
    printf("%s on %s (%dx%d @ %d,%d)\n", 
           cfg.fullscreen ? "Fullscreen" : "Ring",
           qPrintable(target->name()), 
           target->size().width(), target->size().height(),
           target->geometry().x(), target->geometry().y());
    
    // Get Wayland display and bind to layer shell
    wl_display = (struct wl_display*)native->nativeResourceForIntegration("wl_display");
    if (!wl_display) {
        fprintf(stderr, "Not running on Wayland\n");
        return 1;
    }
    
    wl_registry = wl_display_get_registry(wl_display);
    wl_registry_add_listener(wl_registry, &registry_listener, nullptr);
    wl_display_roundtrip(wl_display);
    
    if (!layer_shell) {
        fprintf(stderr, "Compositor doesn't support wlr-layer-shell protocol\n");
        fprintf(stderr, "Make sure you're running KDE Plasma 5.20+ or another compatible compositor\n");
        return 1;
    }
    
    // Create panels
    QList<OverlayPanel*> panels;
    if (cfg.fullscreen) {
        panels << new OverlayPanel(OverlayPanel::Full, 0, color, target);
    } else {
        for (int e = 0; e < 4; e++) {
            panels << new OverlayPanel(OverlayPanel::Edge(e), cfg.width, color, target);
        }
    }
    
    // Show windows first to create the Wayland surfaces
    for (auto *panel : panels) {
        panel->show();
    }
    
    // Process events to ensure surfaces exist
    app.processEvents();
    wl_display_roundtrip(wl_display);
    
    // Setup layer shell for each panel
    for (auto *panel : panels) {
        if (!panel->setupLayerShell()) {
            fprintf(stderr, "Failed to setup layer shell for panel\n");
        }
    }
    
    // Final roundtrip
    wl_display_roundtrip(wl_display);
    
    return app.exec();
}
