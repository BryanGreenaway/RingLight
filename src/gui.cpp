/*
 * RingLight GUI - Settings and control panel for KDE Plasma 6
 * 
 * Features:
 * - Screen selection with per-screen enable
 * - Color and brightness controls  
 * - Howdy integration
 * - System tray with quick toggle
 * - Automatic webcam detection
 *
 * License: MIT
 */

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QColorDialog>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QScreen>
#include <QSettings>
#include <QProcess>
#include <QTimer>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QCloseEvent>
#include <QFrame>
#include <QPainter>
#include <QPixmap>
#include <QPen>
#include <QIcon>

class ColorButton : public QPushButton {
    Q_OBJECT
public:
    ColorButton(QWidget *parent = nullptr) : QPushButton(parent), m_color(Qt::white) {
        setFixedSize(70, 30);
        updateAppearance();
        connect(this, &QPushButton::clicked, this, &ColorButton::chooseColor);
    }
    
    QColor color() const { return m_color; }
    void setColor(const QColor &c) { m_color = c; updateAppearance(); emit colorChanged(c); }
    
signals:
    void colorChanged(const QColor &color);
    
private slots:
    void chooseColor() {
        QColorDialog dlg(m_color, this);
        dlg.setWindowTitle(tr("Ring Light Color"));
        dlg.setOption(QColorDialog::DontUseNativeDialog, false);
        if (dlg.exec() == QDialog::Accepted) {
            setColor(dlg.selectedColor());
        }
    }
    
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        
        // Draw color swatch
        p.setBrush(m_color);
        p.setPen(QPen(Qt::gray, 2));
        p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 4, 4);
    }
    
private:
    void updateAppearance() {
        update(); // Trigger repaint
    }
    QColor m_color;
};

class RingLightGUI : public QMainWindow {
    Q_OBJECT
    
public:
    RingLightGUI(QWidget *parent = nullptr) : QMainWindow(parent) {
        setWindowTitle(tr("RingLight Settings"));
        setMinimumWidth(450);
        
        setupUI();
        setupTray();
        loadSettings();
        
        // Refresh screen list
        refreshScreens();
        
        // Check for video devices
        refreshVideoDevices();
    }
    
    ~RingLightGUI() {
        stopRingLight();
    }
    
protected:
    void closeEvent(QCloseEvent *event) override {
        if (m_tray && m_tray->isVisible() && m_minimizeToTray->isChecked()) {
            hide();
            event->ignore();
        } else {
            saveSettings();
            stopRingLight();
            stopMonitor();
            event->accept();
        }
    }
    
private slots:
    void toggleRingLight() {
        if (m_running) {
            stopRingLight();
        } else {
            startRingLight();
        }
        updateTrayMenu();
    }
    
    void startRingLight() {
        stopRingLight();
        
        QStringList enabledScreenIndices;
        for (int i = 0; i < m_screenList->count(); ++i) {
            QListWidgetItem *item = m_screenList->item(i);
            if (item->checkState() == Qt::Checked) {
                // Store the index, not the name
                enabledScreenIndices << QString::number(item->data(Qt::UserRole + 1).toInt());
            }
        }
        
        if (enabledScreenIndices.isEmpty()) {
            QMessageBox::warning(this, tr("No Screens"), tr("Please select at least one screen."));
            return;
        }
        
        QString color = m_colorBtn->color().name().mid(1); // Remove #
        int brightness = m_brightnessSlider->value();
        int width = m_widthSpin->value();
        
        // Spawn one ringlight-overlay process per screen
        for (const QString &screenIdx : enabledScreenIndices) {
            QProcess *proc = new QProcess(this);
            proc->setProgram(QStringLiteral("ringlight-overlay"));
            proc->setArguments({
                QStringLiteral("-s"), screenIdx,
                QStringLiteral("-c"), color,
                QStringLiteral("-b"), QString::number(brightness),
                QStringLiteral("-w"), QString::number(width)
            });
            proc->start();
            m_overlayProcs << proc;
        }
        
        m_running = true;
        m_toggleBtn->setText(tr("Turn Off"));
        m_toggleBtn->setStyleSheet("background-color: #c0392b; color: white; padding: 10px; font-weight: bold;");
        updateTrayMenu();
    }
    
    void stopRingLight() {
        for (QProcess *proc : m_overlayProcs) {
            proc->terminate();
            proc->waitForFinished(500);
            if (proc->state() != QProcess::NotRunning) {
                proc->kill();
            }
            delete proc;
        }
        m_overlayProcs.clear();
        
        m_running = false;
        m_toggleBtn->setText(tr("Turn On"));
        m_toggleBtn->setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;");
        updateTrayMenu();
    }
    
    void refreshScreens() {
        m_screenList->clear();
        const auto screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            QScreen *s = screens[i];
            QString label = QString("%1: %2 (%3x%4)")
                .arg(i)
                .arg(s->name())
                .arg(s->size().width())
                .arg(s->size().height());
            QListWidgetItem *item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, s->name());      // Store name for settings
            item->setData(Qt::UserRole + 1, i);          // Store index for overlay
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
            m_screenList->addItem(item);
        }
        
        // Check first screen by default if none selected
        if (m_screenList->count() > 0) {
            m_screenList->item(0)->setCheckState(Qt::Checked);
        }
    }
    
    void refreshVideoDevices() {
        m_videoDevice->clear();
        QDir devDir("/dev");
        QStringList videoDevs = devDir.entryList(QStringList() << "video*", QDir::System);
        for (const QString &dev : videoDevs) {
            m_videoDevice->addItem("/dev/" + dev);
        }
        if (m_videoDevice->count() == 0) {
            m_videoDevice->addItem(tr("No webcam found"));
        }
    }
    
    void toggleAutoEnable(bool enabled) {
        m_videoDevice->setEnabled(enabled);
        m_processEdit->setEnabled(enabled);
        
        if (enabled) {
            startMonitor();
        } else {
            stopMonitor();
        }
        saveSettings();
    }
    
    void startMonitor() {
        stopMonitor();
        
        if (!m_autoEnable->isChecked()) return;
        
        // Save settings first so monitor can read them
        saveSettings();
        
        m_monitorProc = new QProcess(this);
        m_monitorProc->setProgram(QStringLiteral("ringlight-monitor"));
        
        // Monitor reads config file by default, just add verbose flag
        m_monitorProc->setArguments({QStringLiteral("-v")});
        
        connect(m_monitorProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
            Q_UNUSED(err);
            m_monitorStatus->setText(tr("Monitor: Error - %1").arg(m_monitorProc->errorString()));
            m_monitorStatus->setStyleSheet("color: #c0392b;");
        });
        
        m_monitorProc->start();
        
        if (m_monitorProc->waitForStarted(1000)) {
            m_monitorStatus->setText(tr("Monitor: Running"));
            m_monitorStatus->setStyleSheet("color: #27ae60;");
        } else {
            m_monitorStatus->setText(tr("Monitor: Failed to start"));
            m_monitorStatus->setStyleSheet("color: #c0392b;");
        }
    }
    
    void stopMonitor() {
        if (m_monitorProc) {
            m_monitorProc->terminate();
            m_monitorProc->waitForFinished(500);
            if (m_monitorProc->state() != QProcess::NotRunning) {
                m_monitorProc->kill();
            }
            delete m_monitorProc;
            m_monitorProc = nullptr;
        }
        m_monitorStatus->setText(tr("Monitor: Stopped"));
        m_monitorStatus->setStyleSheet("color: #888;");
    }
    
    void saveSettings() {
        QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        QDir().mkpath(configPath + "/ringlight");
        QSettings settings(configPath + "/ringlight/config.ini", QSettings::IniFormat);
        
        settings.setValue("color", m_colorBtn->color().name());
        settings.setValue("brightness", m_brightnessSlider->value());
        settings.setValue("width", m_widthSpin->value());
        settings.setValue("autoEnable", m_autoEnable->isChecked());
        settings.setValue("videoDevice", m_videoDevice->currentText());
        settings.setValue("processes", m_processEdit->text());
        settings.setValue("minimizeToTray", m_minimizeToTray->isChecked());
        
        // Save enabled screens (both names for display and indices for overlay)
        QStringList enabledNames;
        QStringList enabledIndices;
        for (int i = 0; i < m_screenList->count(); ++i) {
            QListWidgetItem *item = m_screenList->item(i);
            if (item->checkState() == Qt::Checked) {
                enabledNames << item->data(Qt::UserRole).toString();
                enabledIndices << QString::number(item->data(Qt::UserRole + 1).toInt());
            }
        }
        settings.setValue("enabledScreens", enabledNames.join(','));
        settings.setValue("enabledScreenIndices", enabledIndices.join(','));
        
        settings.sync();
    }
    
    void loadSettings() {
        QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        QSettings settings(configPath + "/ringlight/config.ini", QSettings::IniFormat);
        
        QColor color(settings.value("color", "#FFFFFF").toString());
        m_colorBtn->setColor(color);
        
        m_brightnessSlider->setValue(settings.value("brightness", 100).toInt());
        m_widthSpin->setValue(settings.value("width", 80).toInt());
        m_autoEnable->setChecked(settings.value("autoEnable", false).toBool());
        m_processEdit->setText(settings.value("processes", "howdy").toString());
        m_minimizeToTray->setChecked(settings.value("minimizeToTray", true).toBool());
        
        QString videoDev = settings.value("videoDevice", "/dev/video0").toString();
        int idx = m_videoDevice->findText(videoDev);
        if (idx >= 0) m_videoDevice->setCurrentIndex(idx);
        
        // Get enabled screens from settings
        QString enabledStr = settings.value("enabledScreens", "").toString();
        QStringList enabled = enabledStr.split(',', Qt::SkipEmptyParts);
        
        // Restore enabled screens after refresh (use singleShot to let screen list populate)
        QTimer::singleShot(100, this, [this, enabled]() {
            for (int i = 0; i < m_screenList->count(); ++i) {
                QListWidgetItem *item = m_screenList->item(i);
                QString name = item->data(Qt::UserRole).toString();
                item->setCheckState(enabled.contains(name) ? Qt::Checked : Qt::Unchecked);
            }
            
            // Now safe to toggle auto-enable
            toggleAutoEnable(m_autoEnable->isChecked());
        });
    }
    
    void updateTrayMenu() {
        if (m_toggleAction) {
            m_toggleAction->setText(m_running ? tr("Turn Off") : tr("Turn On"));
        }
    }
    
    void trayActivated(QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            if (isVisible()) {
                hide();
            } else {
                show();
                raise();
                activateWindow();
            }
        }
    }
    
private:
    void setupUI() {
        QWidget *central = new QWidget(this);
        setCentralWidget(central);
        
        QVBoxLayout *mainLayout = new QVBoxLayout(central);
        mainLayout->setSpacing(12);
        
        // === Screens Section ===
        QGroupBox *screenGroup = new QGroupBox(tr("Screens"));
        QVBoxLayout *screenLayout = new QVBoxLayout(screenGroup);
        
        m_screenList = new QListWidget();
        m_screenList->setMaximumHeight(120);
        screenLayout->addWidget(m_screenList);
        
        QPushButton *refreshBtn = new QPushButton(tr("Refresh"));
        refreshBtn->setMaximumWidth(100);
        connect(refreshBtn, &QPushButton::clicked, this, &RingLightGUI::refreshScreens);
        screenLayout->addWidget(refreshBtn);
        
        mainLayout->addWidget(screenGroup);
        
        // === Appearance Section ===
        QGroupBox *appearGroup = new QGroupBox(tr("Appearance"));
        QGridLayout *appearLayout = new QGridLayout(appearGroup);
        
        appearLayout->addWidget(new QLabel(tr("Color:")), 0, 0);
        m_colorBtn = new ColorButton();
        appearLayout->addWidget(m_colorBtn, 0, 1);
        
        appearLayout->addWidget(new QLabel(tr("Brightness:")), 1, 0);
        QHBoxLayout *brightLayout = new QHBoxLayout();
        m_brightnessSlider = new QSlider(Qt::Horizontal);
        m_brightnessSlider->setRange(10, 100);
        m_brightnessSlider->setValue(100);
        m_brightnessLabel = new QLabel("100%");
        m_brightnessLabel->setMinimumWidth(40);
        connect(m_brightnessSlider, &QSlider::valueChanged, this, [this](int v) {
            m_brightnessLabel->setText(QString("%1%").arg(v));
        });
        brightLayout->addWidget(m_brightnessSlider);
        brightLayout->addWidget(m_brightnessLabel);
        appearLayout->addLayout(brightLayout, 1, 1);
        
        appearLayout->addWidget(new QLabel(tr("Width:")), 2, 0);
        m_widthSpin = new QSpinBox();
        m_widthSpin->setRange(10, 300);
        m_widthSpin->setValue(80);
        m_widthSpin->setSuffix(" px");
        appearLayout->addWidget(m_widthSpin, 2, 1);
        
        mainLayout->addWidget(appearGroup);
        
        // === Manual Control ===
        QGroupBox *controlGroup = new QGroupBox(tr("Manual Control"));
        QHBoxLayout *controlLayout = new QHBoxLayout(controlGroup);
        
        m_toggleBtn = new QPushButton(tr("Turn On"));
        m_toggleBtn->setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;");
        connect(m_toggleBtn, &QPushButton::clicked, this, &RingLightGUI::toggleRingLight);
        controlLayout->addWidget(m_toggleBtn);
        
        mainLayout->addWidget(controlGroup);
        
        // === Auto-Enable Section ===
        QGroupBox *autoGroup = new QGroupBox(tr("Automatic Activation"));
        QVBoxLayout *autoLayout = new QVBoxLayout(autoGroup);
        
        m_autoEnable = new QCheckBox(tr("Enable when webcam is active"));
        connect(m_autoEnable, &QCheckBox::toggled, this, &RingLightGUI::toggleAutoEnable);
        autoLayout->addWidget(m_autoEnable);
        
        QGridLayout *autoGrid = new QGridLayout();
        
        autoGrid->addWidget(new QLabel(tr("Video device:")), 0, 0);
        m_videoDevice = new QComboBox();
        autoGrid->addWidget(m_videoDevice, 0, 1);
        
        autoGrid->addWidget(new QLabel(tr("Watch processes:")), 1, 0);
        m_processEdit = new QLineEdit("howdy");
        m_processEdit->setPlaceholderText("howdy, zoom, teams");
        autoGrid->addWidget(m_processEdit, 1, 1);
        
        autoLayout->addLayout(autoGrid);
        
        m_monitorStatus = new QLabel(tr("Monitor: Stopped"));
        m_monitorStatus->setStyleSheet("color: #888;");
        autoLayout->addWidget(m_monitorStatus);
        
        // Initially disable
        m_videoDevice->setEnabled(false);
        m_processEdit->setEnabled(false);
        
        mainLayout->addWidget(autoGroup);
        
        // === Options ===
        QGroupBox *optGroup = new QGroupBox(tr("Options"));
        QVBoxLayout *optLayout = new QVBoxLayout(optGroup);
        
        m_minimizeToTray = new QCheckBox(tr("Minimize to system tray"));
        m_minimizeToTray->setChecked(true);
        optLayout->addWidget(m_minimizeToTray);
        
        mainLayout->addWidget(optGroup);
        
        // === Buttons ===
        QHBoxLayout *btnLayout = new QHBoxLayout();
        
        QPushButton *saveBtn = new QPushButton(tr("Save Settings"));
        connect(saveBtn, &QPushButton::clicked, this, [this]() {
            saveSettings();
            if (m_autoEnable->isChecked()) {
                startMonitor(); // Restart with new settings
            }
        });
        btnLayout->addWidget(saveBtn);
        
        QPushButton *quitBtn = new QPushButton(tr("Quit"));
        connect(quitBtn, &QPushButton::clicked, this, [this]() {
            saveSettings();
            stopRingLight();
            stopMonitor();
            qApp->quit();
        });
        btnLayout->addWidget(quitBtn);
        
        mainLayout->addLayout(btnLayout);
    }
    
    void setupTray() {
        // Check if system tray is available
        if (!QSystemTrayIcon::isSystemTrayAvailable()) {
            m_tray = nullptr;
            return;
        }
        
        m_tray = new QSystemTrayIcon(this);
        
        // Create a simple icon (white circle)
        QPixmap pixmap(32, 32);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(Qt::white);
        painter.setPen(QPen(Qt::gray, 2));
        painter.drawEllipse(2, 2, 28, 28);
        painter.end();
        
        m_tray->setIcon(QIcon(pixmap));
        m_tray->setToolTip(tr("RingLight"));
        
        QMenu *menu = new QMenu();
        
        m_toggleAction = menu->addAction(tr("Turn On"));
        connect(m_toggleAction, &QAction::triggered, this, &RingLightGUI::toggleRingLight);
        
        menu->addSeparator();
        
        QAction *showAction = menu->addAction(tr("Settings..."));
        connect(showAction, &QAction::triggered, this, [this]() {
            show();
            raise();
            activateWindow();
        });
        
        menu->addSeparator();
        
        QAction *quitAction = menu->addAction(tr("Quit"));
        connect(quitAction, &QAction::triggered, this, [this]() {
            saveSettings();
            stopRingLight();
            stopMonitor();
            qApp->quit();
        });
        
        m_tray->setContextMenu(menu);
        
        connect(m_tray, &QSystemTrayIcon::activated, this, &RingLightGUI::trayActivated);
        
        m_tray->show();
    }
    
    // UI elements
    QListWidget *m_screenList;
    ColorButton *m_colorBtn;
    QSlider *m_brightnessSlider;
    QLabel *m_brightnessLabel;
    QSpinBox *m_widthSpin;
    QPushButton *m_toggleBtn;
    QCheckBox *m_autoEnable;
    QComboBox *m_videoDevice;
    QLineEdit *m_processEdit;
    QLabel *m_monitorStatus;
    QCheckBox *m_minimizeToTray;
    
    // Tray
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_toggleAction = nullptr;
    
    // State
    bool m_running = false;
    QList<QProcess*> m_overlayProcs;
    QProcess *m_monitorProc = nullptr;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ringlight-gui"));
    app.setApplicationVersion(QStringLiteral("1.0"));
    app.setQuitOnLastWindowClosed(false);
    
    RingLightGUI gui;
    gui.show();
    
    return app.exec();
}

#include "gui.moc"
