/*
 * RingLight GUI - Settings panel for KDE Plasma 6
 * License: MIT
 */

#include <QApplication>
#include <QMainWindow>
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
#include <QTimer>
#include <QDir>
#include <QStandardPaths>
#include <QCloseEvent>
#include <QPainter>

class ColorButton : public QPushButton {
    Q_OBJECT
public:
    ColorButton(QWidget *parent = nullptr) : QPushButton(parent), m_color(Qt::white) {
        setFixedSize(70, 30);
        connect(this, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(m_color, this, tr("Ring Light Color"));
            if (c.isValid()) { m_color = c; update(); emit colorChanged(c); }
        });
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
private:
    QColor m_color;
};

class RingLightGUI : public QMainWindow {
    Q_OBJECT
public:
    RingLightGUI() {
        setWindowTitle(tr("RingLight Settings"));
        setMinimumWidth(450);
        setupUI();
        setupTray();
        loadSettings();
        refreshScreens();
        refreshVideoDevices();
    }
    ~RingLightGUI() { cleanup(); }

protected:
    void closeEvent(QCloseEvent *e) override {
        if (m_tray && m_tray->isVisible() && m_minimizeToTray->isChecked()) { hide(); e->ignore(); }
        else { cleanup(); e->accept(); }
    }

private:
    void cleanup() { saveSettings(); stopOverlay(); stopMonitor(); }
    
    void stopOverlay() {
        for (QProcess *p : m_overlays) { p->terminate(); p->waitForFinished(500); if (p->state() != QProcess::NotRunning) p->kill(); delete p; }
        m_overlays.clear();
        m_running = false;
        m_toggleBtn->setText(tr("Turn On"));
        m_toggleBtn->setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;");
        if (m_toggleAction) m_toggleAction->setText(tr("Turn On"));
    }
    
    void startOverlay() {
        stopOverlay();
        QStringList baseArgs{"-c", m_colorBtn->color().name().mid(1), "-b", QString::number(m_brightnessSlider->value()), "-w", QString::number(m_widthSpin->value())};
        if (m_fullscreen->isChecked()) baseArgs << "-f";
        
        QStringList names;
        for (int i = 0; i < m_screenList->count(); ++i)
            if (m_screenList->item(i)->checkState() == Qt::Checked) names << m_screenList->item(i)->data(Qt::UserRole).toString();
        if (names.isEmpty()) names << "0";
        
        for (const QString &n : names) {
            auto *p = new QProcess(this);
            p->start("ringlight-overlay", baseArgs + QStringList{"-s", n});
            m_overlays << p;
        }
        m_running = true;
        m_toggleBtn->setText(tr("Turn Off"));
        m_toggleBtn->setStyleSheet("background-color: #c0392b; color: white; padding: 10px; font-weight: bold;");
        if (m_toggleAction) m_toggleAction->setText(tr("Turn Off"));
    }
    
    void stopMonitor() {
        if (m_monitor) { m_monitor->terminate(); m_monitor->waitForFinished(500); if (m_monitor->state() != QProcess::NotRunning) m_monitor->kill(); delete m_monitor; m_monitor = nullptr; }
        m_monitorStatus->setText(tr("Monitor: Stopped"));
        m_monitorStatus->setStyleSheet("color: #888;");
    }
    
    void startMonitor() {
        stopMonitor();
        if (!m_autoEnable->isChecked()) return;
        saveSettings();
        m_monitor = new QProcess(this);
        m_monitor->start("ringlight-monitor", {"-v"});
        m_monitorStatus->setText(m_monitor->waitForStarted(1000) ? tr("Monitor: Running") : tr("Monitor: Failed"));
        m_monitorStatus->setStyleSheet(m_monitor->state() == QProcess::Running ? "color: #27ae60;" : "color: #c0392b;");
    }
    
    void refreshScreens() {
        QStringList prev;
        for (int i = 0; i < m_screenList->count(); ++i) if (m_screenList->item(i)->checkState() == Qt::Checked) prev << m_screenList->item(i)->data(Qt::UserRole).toString();
        m_screenList->clear();
        const auto screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            auto *item = new QListWidgetItem(QString("%1: %2 (%3x%4)").arg(i).arg(screens[i]->name()).arg(screens[i]->size().width()).arg(screens[i]->size().height()));
            item->setData(Qt::UserRole, screens[i]->name());
            item->setData(Qt::UserRole + 1, i);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(prev.contains(screens[i]->name()) ? Qt::Checked : Qt::Unchecked);
            m_screenList->addItem(item);
        }
        bool any = false;
        for (int i = 0; i < m_screenList->count(); ++i) if (m_screenList->item(i)->checkState() == Qt::Checked) { any = true; break; }
        if (!any && m_screenList->count() > 0) m_screenList->item(0)->setCheckState(Qt::Checked);
    }
    
    void refreshVideoDevices() {
        m_videoDevice->clear();
        for (const QString &d : QDir("/dev").entryList({"video*"}, QDir::System)) m_videoDevice->addItem("/dev/" + d);
        if (m_videoDevice->count() == 0) m_videoDevice->addItem(tr("No webcam found"));
    }
    
    void saveSettings() {
        QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        QDir().mkpath(path + "/ringlight");
        QSettings s(path + "/ringlight/config.ini", QSettings::IniFormat);
        s.setValue("color", m_colorBtn->color().name());
        s.setValue("brightness", m_brightnessSlider->value());
        s.setValue("width", m_widthSpin->value());
        s.setValue("fullscreen", m_fullscreen->isChecked());
        s.setValue("autoEnable", m_autoEnable->isChecked());
        s.setValue("videoDevice", m_videoDevice->currentText());
        s.setValue("processes", m_processEdit->text());
        s.setValue("minimizeToTray", m_minimizeToTray->isChecked());
        QStringList names, indices;
        for (int i = 0; i < m_screenList->count(); ++i) {
            auto *item = m_screenList->item(i);
            if (item->checkState() == Qt::Checked) { names << item->data(Qt::UserRole).toString(); indices << QString::number(item->data(Qt::UserRole + 1).toInt()); }
        }
        s.setValue("enabledScreens", names.join(','));
        s.setValue("enabledScreenIndices", indices.join(','));
    }
    
    void loadSettings() {
        QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        QSettings s(path + "/ringlight/config.ini", QSettings::IniFormat);
        m_colorBtn->setColor(QColor(s.value("color", "#FFFFFF").toString()));
        m_brightnessSlider->setValue(s.value("brightness", 100).toInt());
        m_widthSpin->setValue(s.value("width", 80).toInt());
        m_fullscreen->setChecked(s.value("fullscreen", false).toBool());
        m_autoEnable->setChecked(s.value("autoEnable", false).toBool());
        m_processEdit->setText(s.value("processes", "howdy").toString());
        m_minimizeToTray->setChecked(s.value("minimizeToTray", true).toBool());
        int idx = m_videoDevice->findText(s.value("videoDevice", "/dev/video0").toString());
        if (idx >= 0) m_videoDevice->setCurrentIndex(idx);
        QStringList enabled = s.value("enabledScreens", "").toString().split(',', Qt::SkipEmptyParts);
        QTimer::singleShot(100, this, [this, enabled]() {
            if (!enabled.isEmpty()) for (int i = 0; i < m_screenList->count(); ++i) { auto *item = m_screenList->item(i); item->setCheckState(enabled.contains(item->data(Qt::UserRole).toString()) ? Qt::Checked : Qt::Unchecked); }
            toggleAutoEnable(m_autoEnable->isChecked());
        });
        updateWidthEnabled();
    }
    
    void toggleAutoEnable(bool on) { m_videoDevice->setEnabled(on); m_processEdit->setEnabled(on); on ? startMonitor() : stopMonitor(); saveSettings(); }
    void updateWidthEnabled() { m_widthSpin->setEnabled(!m_fullscreen->isChecked()); m_widthLabel->setEnabled(!m_fullscreen->isChecked()); }
    
    void setupUI() {
        auto *central = new QWidget(this); setCentralWidget(central);
        auto *layout = new QVBoxLayout(central); layout->setSpacing(12);
        
        auto *screenGrp = new QGroupBox(tr("Screens")); auto *screenLay = new QVBoxLayout(screenGrp);
        m_screenList = new QListWidget(); m_screenList->setMaximumHeight(120); screenLay->addWidget(m_screenList);
        auto *refreshBtn = new QPushButton(tr("Refresh")); refreshBtn->setMaximumWidth(100);
        connect(refreshBtn, &QPushButton::clicked, this, &RingLightGUI::refreshScreens); screenLay->addWidget(refreshBtn);
        layout->addWidget(screenGrp);
        
        auto *appearGrp = new QGroupBox(tr("Appearance")); auto *appearLay = new QGridLayout(appearGrp);
        appearLay->addWidget(new QLabel(tr("Mode:")), 0, 0);
        m_fullscreen = new QCheckBox(tr("Fullscreen (whole monitor lights up)"));
        connect(m_fullscreen, &QCheckBox::toggled, this, &RingLightGUI::updateWidthEnabled); appearLay->addWidget(m_fullscreen, 0, 1);
        appearLay->addWidget(new QLabel(tr("Color:")), 1, 0); m_colorBtn = new ColorButton(); appearLay->addWidget(m_colorBtn, 1, 1);
        appearLay->addWidget(new QLabel(tr("Brightness:")), 2, 0);
        auto *brightLay = new QHBoxLayout(); m_brightnessSlider = new QSlider(Qt::Horizontal); m_brightnessSlider->setRange(10, 100); m_brightnessSlider->setValue(100);
        m_brightnessLabel = new QLabel("100%"); connect(m_brightnessSlider, &QSlider::valueChanged, this, [this](int v) { m_brightnessLabel->setText(QString("%1%").arg(v)); });
        brightLay->addWidget(m_brightnessSlider); brightLay->addWidget(m_brightnessLabel); appearLay->addLayout(brightLay, 2, 1);
        m_widthLabel = new QLabel(tr("Border Width:")); appearLay->addWidget(m_widthLabel, 3, 0);
        m_widthSpin = new QSpinBox(); m_widthSpin->setRange(10, 300); m_widthSpin->setValue(80); m_widthSpin->setSuffix(" px"); appearLay->addWidget(m_widthSpin, 3, 1);
        layout->addWidget(appearGrp);
        
        auto *ctrlGrp = new QGroupBox(tr("Manual Control")); auto *ctrlLay = new QHBoxLayout(ctrlGrp);
        m_toggleBtn = new QPushButton(tr("Turn On")); m_toggleBtn->setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;");
        connect(m_toggleBtn, &QPushButton::clicked, this, [this]() { m_running ? stopOverlay() : startOverlay(); }); ctrlLay->addWidget(m_toggleBtn);
        layout->addWidget(ctrlGrp);
        
        auto *autoGrp = new QGroupBox(tr("Automatic Activation")); auto *autoLay = new QVBoxLayout(autoGrp);
        m_autoEnable = new QCheckBox(tr("Enable when webcam is active")); connect(m_autoEnable, &QCheckBox::toggled, this, &RingLightGUI::toggleAutoEnable); autoLay->addWidget(m_autoEnable);
        auto *autoGrid = new QGridLayout();
        autoGrid->addWidget(new QLabel(tr("Video device:")), 0, 0); m_videoDevice = new QComboBox(); m_videoDevice->setEnabled(false); autoGrid->addWidget(m_videoDevice, 0, 1);
        autoGrid->addWidget(new QLabel(tr("Watch processes:")), 1, 0); m_processEdit = new QLineEdit("howdy"); m_processEdit->setEnabled(false); autoGrid->addWidget(m_processEdit, 1, 1);
        autoLay->addLayout(autoGrid);
        m_monitorStatus = new QLabel(tr("Monitor: Stopped")); m_monitorStatus->setStyleSheet("color: #888;"); autoLay->addWidget(m_monitorStatus);
        layout->addWidget(autoGrp);
        
        auto *optGrp = new QGroupBox(tr("Options")); auto *optLay = new QVBoxLayout(optGrp);
        m_minimizeToTray = new QCheckBox(tr("Minimize to system tray")); m_minimizeToTray->setChecked(true); optLay->addWidget(m_minimizeToTray);
        layout->addWidget(optGrp);
        
        auto *btnLay = new QHBoxLayout();
        auto *saveBtn = new QPushButton(tr("Save Settings")); connect(saveBtn, &QPushButton::clicked, this, [this]() { saveSettings(); if (m_autoEnable->isChecked()) startMonitor(); }); btnLay->addWidget(saveBtn);
        auto *quitBtn = new QPushButton(tr("Quit")); connect(quitBtn, &QPushButton::clicked, this, [this]() { cleanup(); qApp->quit(); }); btnLay->addWidget(quitBtn);
        layout->addLayout(btnLay);
    }
    
    void setupTray() {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
        m_tray = new QSystemTrayIcon(this);
        QPixmap px(32, 32); px.fill(Qt::transparent);
        QPainter p(&px); p.setRenderHint(QPainter::Antialiasing); p.setBrush(Qt::white); p.setPen(QPen(Qt::gray, 2)); p.drawEllipse(2, 2, 28, 28);
        m_tray->setIcon(QIcon(px)); m_tray->setToolTip(tr("RingLight"));
        auto *menu = new QMenu();
        m_toggleAction = menu->addAction(tr("Turn On")); connect(m_toggleAction, &QAction::triggered, this, [this]() { m_running ? stopOverlay() : startOverlay(); });
        menu->addSeparator(); connect(menu->addAction(tr("Settings...")), &QAction::triggered, this, [this]() { show(); raise(); activateWindow(); });
        menu->addSeparator(); connect(menu->addAction(tr("Quit")), &QAction::triggered, this, [this]() { cleanup(); qApp->quit(); });
        m_tray->setContextMenu(menu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) { if (r == QSystemTrayIcon::Trigger) isVisible() ? hide() : (show(), raise(), activateWindow()); });
        m_tray->show();
    }
    
    QListWidget *m_screenList; ColorButton *m_colorBtn; QSlider *m_brightnessSlider; QLabel *m_brightnessLabel;
    QSpinBox *m_widthSpin; QLabel *m_widthLabel; QCheckBox *m_fullscreen; QPushButton *m_toggleBtn;
    QCheckBox *m_autoEnable; QComboBox *m_videoDevice; QLineEdit *m_processEdit; QLabel *m_monitorStatus;
    QCheckBox *m_minimizeToTray; QSystemTrayIcon *m_tray = nullptr; QAction *m_toggleAction = nullptr;
    bool m_running = false; QProcess *m_monitor = nullptr; QList<QProcess*> m_overlays;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("ringlight-gui");
    app.setQuitOnLastWindowClosed(false);
    RingLightGUI gui;
    gui.show();
    return app.exec();
}

#include "gui.moc"
