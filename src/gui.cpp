/*
 * RingLight GUI - Settings panel with system tray
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
#include <QScreen>
#include <QSettings>
#include <QProcess>
#include <QDir>
#include <QStandardPaths>
#include <QCloseEvent>
#include <QPainter>

class ColorButton : public QWidget {
    Q_OBJECT
public:
    explicit ColorButton(QWidget *parent = nullptr) : QWidget(parent), m_color(Qt::white) {
        setFixedSize(60, 30);
        setCursor(Qt::PointingHandCursor);
    }
    QColor color() const { return m_color; }
    void setColor(const QColor &c) { m_color = c; update(); emit colorChanged(c); }
signals:
    void colorChanged(const QColor &color);
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(m_color);
        p.setPen(QPen(Qt::gray, 2));
        p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 4, 4);
    }
    void mousePressEvent(QMouseEvent *) override {
        QColor c = QColorDialog::getColor(m_color, this, tr("Select Color"));
        if (c.isValid()) setColor(c);
    }
private:
    QColor m_color;
};

class RingLightGUI : public QMainWindow {
    Q_OBJECT
public:
    RingLightGUI() {
        setWindowTitle(tr("RingLight"));
        setMinimumWidth(420);
        setupUI();
        setupTray();
        loadSettings();
        refreshScreens();
    }
    
    ~RingLightGUI() { cleanup(); }

protected:
    void closeEvent(QCloseEvent *e) override {
        if (m_tray && m_tray->isVisible() && m_minimizeToTray->isChecked()) {
            hide();
            e->ignore();
        } else {
            cleanup();
            e->accept();
        }
    }

private:
    void cleanup() {
        saveSettings();
        stopOverlay();
        stopMonitor();
    }
    
    void setupUI() {
        auto *central = new QWidget();
        setCentralWidget(central);
        auto *layout = new QVBoxLayout(central);
        layout->setSpacing(12);
        
        // Appearance
        auto *appearGrp = new QGroupBox(tr("Appearance"));
        auto *appearLay = new QGridLayout(appearGrp);
        
        appearLay->addWidget(new QLabel(tr("Color:")), 0, 0);
        m_colorBtn = new ColorButton();
        appearLay->addWidget(m_colorBtn, 0, 1, Qt::AlignLeft);
        
        appearLay->addWidget(new QLabel(tr("Brightness:")), 1, 0);
        auto *brightLay = new QHBoxLayout();
        m_brightnessSlider = new QSlider(Qt::Horizontal);
        m_brightnessSlider->setRange(10, 100);
        m_brightnessSlider->setValue(100);
        m_brightnessLabel = new QLabel("100%");
        m_brightnessLabel->setFixedWidth(40);
        connect(m_brightnessSlider, &QSlider::valueChanged, this, [this](int v) {
            m_brightnessLabel->setText(QString("%1%").arg(v));
        });
        brightLay->addWidget(m_brightnessSlider);
        brightLay->addWidget(m_brightnessLabel);
        appearLay->addLayout(brightLay, 1, 1);
        
        appearLay->addWidget(new QLabel(tr("Width:")), 2, 0);
        m_widthSpin = new QSpinBox();
        m_widthSpin->setRange(10, 500);
        m_widthSpin->setValue(80);
        m_widthSpin->setSuffix(" px");
        appearLay->addWidget(m_widthSpin, 2, 1);
        
        m_fullscreen = new QCheckBox(tr("Fullscreen mode"));
        appearLay->addWidget(m_fullscreen, 3, 0, 1, 2);
        
        layout->addWidget(appearGrp);
        
        // Screens
        auto *screenGrp = new QGroupBox(tr("Screens"));
        auto *screenLay = new QVBoxLayout(screenGrp);
        m_screenList = new QListWidget();
        m_screenList->setMaximumHeight(100);
        screenLay->addWidget(m_screenList);
        auto *refreshBtn = new QPushButton(tr("Refresh"));
        connect(refreshBtn, &QPushButton::clicked, this, &RingLightGUI::refreshScreens);
        screenLay->addWidget(refreshBtn);
        layout->addWidget(screenGrp);
        
        // Manual control
        auto *ctrlGrp = new QGroupBox(tr("Manual Control"));
        auto *ctrlLay = new QVBoxLayout(ctrlGrp);
        m_toggleBtn = new QPushButton(tr("Turn On"));
        m_toggleBtn->setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;");
        connect(m_toggleBtn, &QPushButton::clicked, this, [this]() {
            m_running ? stopOverlay() : startOverlay();
        });
        ctrlLay->addWidget(m_toggleBtn);
        layout->addWidget(ctrlGrp);
        
        // Auto activation
        auto *autoGrp = new QGroupBox(tr("Automatic Activation"));
        auto *autoLay = new QVBoxLayout(autoGrp);
        
        m_autoEnable = new QCheckBox(tr("Enable monitor daemon"));
        connect(m_autoEnable, &QCheckBox::toggled, this, &RingLightGUI::onAutoEnableToggled);
        autoLay->addWidget(m_autoEnable);
        
        auto *autoGrid = new QGridLayout();
        
        autoGrid->addWidget(new QLabel(tr("Mode:")), 0, 0);
        m_modeCombo = new QComboBox();
        m_modeCombo->addItem(tr("Process (no polling)"), "process");
        m_modeCombo->addItem(tr("Camera (polling)"), "camera");
        m_modeCombo->addItem(tr("Hybrid (both)"), "hybrid");
        connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &RingLightGUI::onModeChanged);
        autoGrid->addWidget(m_modeCombo, 0, 1);
        
        autoGrid->addWidget(new QLabel(tr("Watch processes:")), 1, 0);
        m_processEdit = new QLineEdit("howdy");
        m_processEdit->setPlaceholderText("howdy, cheese, obs");
        autoGrid->addWidget(m_processEdit, 1, 1);
        
        m_pollLabel = new QLabel(tr("Poll interval:"));
        autoGrid->addWidget(m_pollLabel, 2, 0);
        m_pollSpin = new QSpinBox();
        m_pollSpin->setRange(100, 10000);
        m_pollSpin->setValue(2000);
        m_pollSpin->setSuffix(" ms");
        autoGrid->addWidget(m_pollSpin, 2, 1);
        
        m_videoLabel = new QLabel(tr("Video device:"));
        autoGrid->addWidget(m_videoLabel, 3, 0);
        m_videoDevice = new QComboBox();
        m_videoDevice->setEditable(true);
        refreshVideoDevices();
        autoGrid->addWidget(m_videoDevice, 3, 1);
        
        autoLay->addLayout(autoGrid);
        
        m_monitorStatus = new QLabel(tr("Monitor: Stopped"));
        m_monitorStatus->setStyleSheet("color: #888;");
        autoLay->addWidget(m_monitorStatus);
        
        layout->addWidget(autoGrp);
        
        // Options
        auto *optGrp = new QGroupBox(tr("Options"));
        auto *optLay = new QVBoxLayout(optGrp);
        m_minimizeToTray = new QCheckBox(tr("Minimize to system tray"));
        m_minimizeToTray->setChecked(true);
        optLay->addWidget(m_minimizeToTray);
        layout->addWidget(optGrp);
        
        // Buttons
        auto *btnLay = new QHBoxLayout();
        auto *saveBtn = new QPushButton(tr("Save && Apply"));
        connect(saveBtn, &QPushButton::clicked, this, [this]() {
            saveSettings();
            if (m_autoEnable->isChecked()) {
                stopMonitor();
                startMonitor();
            }
        });
        btnLay->addWidget(saveBtn);
        
        auto *quitBtn = new QPushButton(tr("Quit"));
        connect(quitBtn, &QPushButton::clicked, this, [this]() {
            cleanup();
            qApp->quit();
        });
        btnLay->addWidget(quitBtn);
        layout->addLayout(btnLay);
        
        // Initial state
        onAutoEnableToggled(false);
        onModeChanged(0);
    }
    
    void setupTray() {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
        
        m_tray = new QSystemTrayIcon(this);
        QPixmap px(32, 32);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(Qt::white);
        p.setPen(QPen(Qt::gray, 2));
        p.drawEllipse(2, 2, 28, 28);
        m_tray->setIcon(QIcon(px));
        m_tray->setToolTip(tr("RingLight"));
        
        auto *menu = new QMenu();
        m_toggleAction = menu->addAction(tr("Turn On"));
        connect(m_toggleAction, &QAction::triggered, this, [this]() {
            m_running ? stopOverlay() : startOverlay();
        });
        menu->addSeparator();
        connect(menu->addAction(tr("Settings...")), &QAction::triggered, this, [this]() {
            show();
            raise();
            activateWindow();
        });
        menu->addSeparator();
        connect(menu->addAction(tr("Quit")), &QAction::triggered, this, [this]() {
            cleanup();
            qApp->quit();
        });
        
        m_tray->setContextMenu(menu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger) {
                isVisible() ? hide() : show();
            }
        });
        m_tray->show();
    }
    
    void onAutoEnableToggled(bool enabled) {
        m_modeCombo->setEnabled(enabled);
        m_processEdit->setEnabled(enabled);
        m_pollSpin->setEnabled(enabled);
        m_videoDevice->setEnabled(enabled);
        
        if (!enabled) stopMonitor();
    }
    
    void onModeChanged(int) {
        QString mode = m_modeCombo->currentData().toString();
        bool needsPoll = (mode == "camera" || mode == "hybrid");
        bool needsProc = (mode == "process" || mode == "hybrid");
        
        m_pollLabel->setVisible(needsPoll);
        m_pollSpin->setVisible(needsPoll);
        m_videoLabel->setVisible(needsPoll);
        m_videoDevice->setVisible(needsPoll);
        m_processEdit->setEnabled(needsProc && m_autoEnable->isChecked());
    }
    
    void refreshScreens() {
        m_screenList->clear();
        const auto screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            QScreen *scr = screens[i];
            QString name = QString("%1: %2 (%3x%4)")
                .arg(i)
                .arg(scr->name())
                .arg(scr->size().width())
                .arg(scr->size().height());
            auto *item = new QListWidgetItem(name);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(i == 0 ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, i);
            m_screenList->addItem(item);
        }
    }
    
    void refreshVideoDevices() {
        m_videoDevice->clear();
        QDir dev("/dev");
        QStringList videos = dev.entryList(QStringList() << "video*", QDir::System);
        for (const QString &v : videos) {
            m_videoDevice->addItem("/dev/" + v);
        }
        if (m_videoDevice->count() == 0) {
            m_videoDevice->addItem("/dev/video0");
        }
    }
    
    QStringList getEnabledScreens() {
        QStringList screens;
        for (int i = 0; i < m_screenList->count(); ++i) {
            auto *item = m_screenList->item(i);
            if (item->checkState() == Qt::Checked) {
                screens << item->data(Qt::UserRole).toString();
            }
        }
        return screens;
    }
    
    void startOverlay() {
        stopOverlay();
        
        QStringList screens = getEnabledScreens();
        if (screens.isEmpty()) return;
        
        for (const QString &scr : screens) {
            auto *proc = new QProcess(this);
            QStringList args;
            args << "-s" << scr;
            args << "-c" << m_colorBtn->color().name().mid(1);
            args << "-b" << QString::number(m_brightnessSlider->value());
            args << "-w" << QString::number(m_widthSpin->value());
            if (m_fullscreen->isChecked()) args << "-f";
            
            proc->start("ringlight-overlay", args);
            m_overlayProcs.append(proc);
        }
        
        m_running = true;
        m_toggleBtn->setText(tr("Turn Off"));
        m_toggleBtn->setStyleSheet("background-color: #c0392b; color: white; padding: 10px; font-weight: bold;");
        if (m_toggleAction) m_toggleAction->setText(tr("Turn Off"));
    }
    
    void stopOverlay() {
        for (QProcess *p : m_overlayProcs) {
            p->terminate();
            p->waitForFinished(300);
            if (p->state() != QProcess::NotRunning) p->kill();
            delete p;
        }
        m_overlayProcs.clear();
        
        m_running = false;
        m_toggleBtn->setText(tr("Turn On"));
        m_toggleBtn->setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;");
        if (m_toggleAction) m_toggleAction->setText(tr("Turn On"));
    }
    
    void startMonitor() {
        stopMonitor();
        
        m_monitorProc = new QProcess(this);
        QStringList args;
        
        QString mode = m_modeCombo->currentData().toString();
        args << "-m" << mode;
        
        if (mode == "process" || mode == "hybrid") {
            QStringList procs = m_processEdit->text().split(',', Qt::SkipEmptyParts);
            for (QString p : procs) {
                args << "-p" << p.trimmed();
            }
        }
        
        if (mode == "camera" || mode == "hybrid") {
            args << "-d" << m_videoDevice->currentText();
            args << "-i" << QString::number(m_pollSpin->value());
        }
        
        connect(m_monitorProc, &QProcess::started, this, [this]() {
            m_monitorStatus->setText(tr("Monitor: Running"));
            m_monitorStatus->setStyleSheet("color: #27ae60;");
        });
        connect(m_monitorProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus) {
            m_monitorStatus->setText(tr("Monitor: Stopped (exit %1)").arg(code));
            m_monitorStatus->setStyleSheet("color: #c0392b;");
        });
        
        m_monitorProc->start("ringlight-monitor", args);
    }
    
    void stopMonitor() {
        if (m_monitorProc) {
            m_monitorProc->terminate();
            m_monitorProc->waitForFinished(500);
            if (m_monitorProc->state() != QProcess::NotRunning) m_monitorProc->kill();
            delete m_monitorProc;
            m_monitorProc = nullptr;
        }
        m_monitorStatus->setText(tr("Monitor: Stopped"));
        m_monitorStatus->setStyleSheet("color: #888;");
    }
    
    void saveSettings() {
        QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/ringlight";
        QDir().mkpath(configDir);
        
        // Save Qt settings
        QSettings settings(configDir + "/gui.ini", QSettings::IniFormat);
        settings.setValue("color", m_colorBtn->color().name());
        settings.setValue("brightness", m_brightnessSlider->value());
        settings.setValue("width", m_widthSpin->value());
        settings.setValue("fullscreen", m_fullscreen->isChecked());
        settings.setValue("minimizeToTray", m_minimizeToTray->isChecked());
        settings.setValue("autoEnable", m_autoEnable->isChecked());
        settings.setValue("mode", m_modeCombo->currentData().toString());
        settings.setValue("watchProcesses", m_processEdit->text());
        settings.setValue("pollInterval", m_pollSpin->value());
        settings.setValue("videoDevice", m_videoDevice->currentText());
        
        // Save enabled screens
        QStringList screens = getEnabledScreens();
        settings.setValue("screens", screens.join(","));
        
        // Write config.ini for monitor daemon
        QFile configFile(configDir + "/config.ini");
        if (configFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&configFile);
            out << "[monitor]\n";
            out << "mode=" << m_modeCombo->currentData().toString() << "\n";
            out << "watch_processes=" << m_processEdit->text() << "\n";
            out << "poll_interval=" << m_pollSpin->value() << "\n";
            out << "video_device=" << m_videoDevice->currentText() << "\n";
            out << "\n[overlay]\n";
            out << "color=" << m_colorBtn->color().name().mid(1) << "\n";
            out << "brightness=" << m_brightnessSlider->value() << "\n";
            out << "width=" << m_widthSpin->value() << "\n";
            out << "fullscreen=" << (m_fullscreen->isChecked() ? "true" : "false") << "\n";
            out << "screens=" << screens.join(",") << "\n";
            configFile.close();
        }
    }
    
    void loadSettings() {
        QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/ringlight";
        QSettings settings(configDir + "/gui.ini", QSettings::IniFormat);
        
        m_colorBtn->setColor(QColor(settings.value("color", "#FFFFFF").toString()));
        m_brightnessSlider->setValue(settings.value("brightness", 100).toInt());
        m_widthSpin->setValue(settings.value("width", 80).toInt());
        m_fullscreen->setChecked(settings.value("fullscreen", false).toBool());
        m_minimizeToTray->setChecked(settings.value("minimizeToTray", true).toBool());
        m_autoEnable->setChecked(settings.value("autoEnable", false).toBool());
        m_processEdit->setText(settings.value("watchProcesses", "howdy").toString());
        m_pollSpin->setValue(settings.value("pollInterval", 2000).toInt());
        
        QString mode = settings.value("mode", "process").toString();
        int modeIdx = m_modeCombo->findData(mode);
        if (modeIdx >= 0) m_modeCombo->setCurrentIndex(modeIdx);
        
        QString videoDevice = settings.value("videoDevice", "/dev/video0").toString();
        int vidIdx = m_videoDevice->findText(videoDevice);
        if (vidIdx >= 0) m_videoDevice->setCurrentIndex(vidIdx);
        else m_videoDevice->setCurrentText(videoDevice);
        
        // Load screens
        QString screenStr = settings.value("screens", "0").toString();
        QStringList enabledScreens = screenStr.split(',', Qt::SkipEmptyParts);
        for (int i = 0; i < m_screenList->count(); ++i) {
            auto *item = m_screenList->item(i);
            QString idx = item->data(Qt::UserRole).toString();
            item->setCheckState(enabledScreens.contains(idx) ? Qt::Checked : Qt::Unchecked);
        }
        
        onAutoEnableToggled(m_autoEnable->isChecked());
        onModeChanged(m_modeCombo->currentIndex());
        
        // Auto-start monitor if enabled
        if (m_autoEnable->isChecked()) {
            startMonitor();
        }
    }
    
    // UI
    QListWidget *m_screenList = nullptr;
    ColorButton *m_colorBtn = nullptr;
    QSlider *m_brightnessSlider = nullptr;
    QLabel *m_brightnessLabel = nullptr;
    QSpinBox *m_widthSpin = nullptr;
    QCheckBox *m_fullscreen = nullptr;
    QPushButton *m_toggleBtn = nullptr;
    QCheckBox *m_autoEnable = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QLineEdit *m_processEdit = nullptr;
    QLabel *m_pollLabel = nullptr;
    QSpinBox *m_pollSpin = nullptr;
    QLabel *m_videoLabel = nullptr;
    QComboBox *m_videoDevice = nullptr;
    QLabel *m_monitorStatus = nullptr;
    QCheckBox *m_minimizeToTray = nullptr;
    
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
    app.setApplicationName("ringlight-gui");
    app.setApplicationVersion("1.0");
    app.setQuitOnLastWindowClosed(false);
    
    RingLightGUI gui;
    gui.show();
    
    return app.exec();
}

#include "gui.moc"
