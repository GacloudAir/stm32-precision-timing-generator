#include "ui/mainwindow.h"

#include "core/device.h"
#include "core/models.h"

#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QWidget* makeValueLabel(QLabel*& out)
{
    out = new QLabel("--");
    out->setObjectName("value");
    QFont f = out->font();
    f.setPointSize(f.pointSize() + 3);
    f.setBold(true);
    out->setFont(f);
    return out;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_dev(new TimingDevice(this))
{
    setWindowTitle("STM32 精密时序发生器 — 上位机");
    resize(580, 660);
    buildUi();
    refreshPorts();
    setConnectedUi(false);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(1000);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::onPollTimer);
    m_pollTimer->start();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    // 所有颜色都显式写出，不依赖系统主题，彻底避开深/浅色模式导致的白底白字。
    // Every colour is stated explicitly; nothing is inherited from the system
    // theme, which is what caused white text on white backgrounds.
    setStyleSheet(R"(
        QWidget { color: #1F2937; }
        QWidget#central { background: #F5F7FA; }
        QLabel { color: #1F2937; background: transparent; }

        QGroupBox { background: #FFFFFF; border: 1px solid #E2E8F0; border-radius: 10px;
                    margin-top: 12px; padding: 14px 14px 10px 14px;
                    font-weight: 600; color: #334155; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; top: 0px;
                           padding: 0 6px; color: #2563EB; background: #FFFFFF; }

        QPushButton { padding: 7px 16px; border: 1px solid #CBD5E1; border-radius: 7px;
                      background: #FFFFFF; color: #1F2937; }
        QPushButton:hover    { background: #F1F5F9; border-color: #94A3B8; }
        QPushButton:pressed  { background: #E2E8F0; }
        QPushButton:disabled { background: #F8FAFC; color: #94A3B8; border-color: #E2E8F0; }

        QPushButton#primary       { background: #2563EB; color: #FFFFFF;
                                    border: 1px solid #1D4ED8; font-weight: 600; }
        QPushButton#primary:hover { background: #3B76E8; }
        QPushButton#primary:pressed { background: #1D4ED8; }
        QPushButton#primary:disabled { background: #9DBBF2; color: #EEF4FF;
                                       border-color: #9DBBF2; }

        QPushButton#accent       { background: #0D9488; color: #FFFFFF;
                                   border: 1px solid #0F766E; font-weight: 600; }
        QPushButton#accent:hover { background: #14A99B; }
        QPushButton#accent:pressed { background: #0F766E; }
        QPushButton#accent:disabled { background: #92D2CA; color: #F0FDFA;
                                      border-color: #92D2CA; }

        QPushButton#danger       { background: #DC2626; color: #FFFFFF;
                                   border: 1px solid #B91C1C; font-weight: 600; }
        QPushButton#danger:hover { background: #E54A4A; }
        QPushButton#danger:pressed { background: #B91C1C; }
        QPushButton#danger:disabled { background: #F0ACAC; color: #FFF5F5;
                                      border-color: #F0ACAC; }

        QLineEdit, QComboBox { padding: 6px 10px; border: 1px solid #CBD5E1;
                               border-radius: 7px; background: #FFFFFF; color: #1F2937;
                               selection-background-color: #2563EB; selection-color: #FFFFFF; }
        QLineEdit:focus, QComboBox:focus { border-color: #2563EB; }
        QComboBox::drop-down { border: none; width: 22px; }
        QComboBox QAbstractItemView { background: #FFFFFF; color: #1F2937;
                                      border: 1px solid #CBD5E1;
                                      selection-background-color: #2563EB;
                                      selection-color: #FFFFFF; }

        QPlainTextEdit { background: #FFFFFF; color: #1F2937; border: 1px solid #E2E8F0;
                         border-radius: 8px; padding: 8px;
                         font-family: "Consolas", "Microsoft YaHei UI", monospace;
                         font-size: 12px; }
        QScrollBar:vertical { background: #F1F5F9; width: 10px; }
        QScrollBar::handle:vertical { background: #CBD5E1; border-radius: 5px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
    )");

    auto* central = new QWidget(this);
    central->setObjectName("central");
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(16, 14, 16, 16);
    root->setSpacing(10);

    // ---------------- 连接 ----------------
    auto* connBox = new QGroupBox("连接", central);
    auto* connLay = new QHBoxLayout(connBox);
    m_portBox = new QComboBox(connBox);
    m_portBox->setMinimumWidth(230);
    auto* refreshBtn = new QPushButton("刷新", connBox);
    m_connBtn = new QPushButton("连接", connBox);
    m_connBtn->setObjectName("primary");
    m_connLabel = new QLabel("● 未连接", connBox);
    connLay->addWidget(new QLabel("串口:", connBox));
    connLay->addWidget(m_portBox, 1);
    connLay->addWidget(refreshBtn);
    connLay->addWidget(m_connBtn);
    connLay->addWidget(m_connLabel);
    root->addWidget(connBox);

    // ---------------- 控制 ----------------
    auto* ctrlBox = new QGroupBox("控制", central);
    auto* ctrlLay = new QVBoxLayout(ctrlBox);

    auto* freqRow = new QHBoxLayout();
    m_freqEdit = new QLineEdit("166666", ctrlBox);
    m_freqEdit->setMaximumWidth(140);
    m_setBtn = new QPushButton("设置频率", ctrlBox);
    m_setBtn->setObjectName("primary");
    m_selfTestBtn = new QPushButton("自检 (SELF_TEST)", ctrlBox);
    m_selfTestBtn->setObjectName("accent");
    freqRow->addWidget(new QLabel("频率(Hz):", ctrlBox));
    freqRow->addWidget(m_freqEdit);
    freqRow->addWidget(m_setBtn);
    freqRow->addStretch(1);
    freqRow->addWidget(m_selfTestBtn);
    ctrlLay->addLayout(freqRow);

    auto* runRow = new QHBoxLayout();
    m_startBtn = new QPushButton("启动输出", ctrlBox);
    m_startBtn->setObjectName("primary");
    m_stopBtn = new QPushButton("停止输出", ctrlBox);
    m_stopBtn->setObjectName("danger");
    runRow->addWidget(m_startBtn);
    runRow->addWidget(m_stopBtn);
    runRow->addStretch(1);
    ctrlLay->addLayout(runRow);
    root->addWidget(ctrlBox);

    // ---------------- 状态 ----------------
    auto* stBox = new QGroupBox("状态", central);
    auto* grid = new QGridLayout(stBox);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(8);
    grid->addWidget(new QLabel("运行状态", stBox), 0, 0);
    grid->addWidget(makeValueLabel(m_runValue), 0, 1);
    grid->addWidget(new QLabel("当前频率", stBox), 1, 0);
    grid->addWidget(makeValueLabel(m_freqValue), 1, 1);
    grid->addWidget(new QLabel("实测误差", stBox), 2, 0);
    grid->addWidget(makeValueLabel(m_errValue), 2, 1);
    grid->setColumnStretch(1, 1);
    root->addWidget(stBox);

    // ---------------- 日志 ----------------
    auto* logBox = new QGroupBox("日志", central);
    auto* logLay = new QVBoxLayout(logBox);
    m_log = new QPlainTextEdit(logBox);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    m_log->setMinimumHeight(180);
    logLay->addWidget(m_log);
    root->addWidget(logBox, 1);

    setCentralWidget(central);

    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(m_connBtn, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    connect(m_setBtn, &QPushButton::clicked, this, &MainWindow::applyFrequency);
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::doStart);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::doStop);
    connect(m_selfTestBtn, &QPushButton::clicked, this, &MainWindow::doSelfTest);
}

void MainWindow::log(const QString& msg)
{
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_log->appendPlainText(QString("[%1] %2").arg(ts, msg));
}

void MainWindow::guarded(const QString& what, const std::function<void()>& fn)
{
    try {
        fn();
    } catch (const DeviceError& e) {
        const QString msg = QString::fromUtf8(e.what());
        log(QString("%1失败: %2").arg(what, msg));
        QMessageBox::warning(this, what + "失败", msg);
    } catch (const std::exception& e) {
        const QString msg = QString::fromUtf8(e.what());
        log(QString("%1异常: %2").arg(what, msg));
        QMessageBox::warning(this, what + "异常", msg);
    }
}

void MainWindow::refreshPorts()
{
    m_portBox->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& info : ports) {
        QString label = info.portName();
        if (!info.description().isEmpty())
            label += "  —  " + info.description();
        m_portBox->addItem(label, info.portName());
    }
    log(QString("发现 %1 个串口").arg(m_portBox->count()));
}

void MainWindow::setConnectedUi(bool connected)
{
    m_connLabel->setText(connected ? "● 已连接" : "● 未连接");
    m_connLabel->setStyleSheet(connected ? "color:#059669; font-weight:600;"
                                         : "color:#DC2626; font-weight:600;");
    m_connBtn->setText(connected ? "断开" : "连接");
    m_portBox->setEnabled(!connected);
    m_setBtn->setEnabled(connected);
    m_startBtn->setEnabled(connected);
    m_stopBtn->setEnabled(connected);
    m_selfTestBtn->setEnabled(connected);
}

void MainWindow::refreshErrLabel()
{
    // 中文：误差只认 SELF_TEST 的返回值，绝不从 GET_STATUS 的预留字段取。
    // EN: The error label reflects SELF_TEST only, never GET_STATUS.err
    //     (which the firmware reserves and leaves at 0).
    m_errValue->setText(m_lastSelfTestErr < 0
                            ? "--"
                            : QString::number(m_lastSelfTestErr) + " ‰");
}

void MainWindow::refreshValueStyles()
{
    const bool running = m_runValue->text() == "运行中";
    m_runValue->setStyleSheet(QString("QLabel { color: %1; }")
                                  .arg(running ? "#059669" : "#64748B"));

    const QString txt = m_errValue->text();   // 例如 "6 ‰" / "--"
    QString color = "#64748B";
    if (txt != "--") {
        bool ok = false;
        const int err = txt.chopped(2).toInt(&ok);   // 去掉 " ‰"
        if (ok) color = (err == 0) ? "#059669" : "#D97706";
    }
    m_errValue->setStyleSheet(QString("QLabel { color: %1; }").arg(color));
}

void MainWindow::toggleConnection()
{
    if (m_dev->isConnected()) {
        m_dev->disconnectPort();
        setConnectedUi(false);
        m_runValue->setText("--");
        m_freqValue->setText("--");
        m_lastSelfTestErr = -1;
        refreshErrLabel();
        refreshValueStyles();
        log("已断开");
        return;
    }

    const QString port = m_portBox->currentData().toString();
    if (port.isEmpty()) {
        QMessageBox::information(this, "提示", "没有可用串口，请先插入 CH340 并点“刷新”。");
        return;
    }

    guarded("连接", [&] {
        m_dev->connectPort(port, 115200);
        log("已连接 " + port + " @115200 8N1");
        setConnectedUi(true);
    });
    if (m_dev->isConnected())
        doRefreshStatus();
}

void MainWindow::applyFrequency()
{
    bool ok = false;
    const int hz = m_freqEdit->text().toInt(&ok);
    if (!ok || hz <= 0) {
        QMessageBox::warning(this, "输入有误", "请输入正整数频率（Hz）。");
        return;
    }
    guarded("设置频率", [&] {
        m_dev->setFreq(hz);
        m_lastSelfTestErr = -1;   // 频率变了，旧的实测误差失效
        refreshErrLabel();
        refreshValueStyles();
        log(QString("设置频率 %1 Hz").arg(hz));
    });
    doRefreshStatus();
}

void MainWindow::doStart()
{
    guarded("启动", [&] {
        m_dev->start();
        log("已启动输出");
    });
    doRefreshStatus();
}

void MainWindow::doStop()
{
    guarded("停止", [&] {
        m_dev->stop();
        log("已停止输出");
    });
    doRefreshStatus();
}

void MainWindow::doSelfTest()
{
    guarded("自检", [&] {
        const int err = m_dev->selfTest();
        m_lastSelfTestErr = err;
        refreshErrLabel();
        refreshValueStyles();
        log(QString("SELF_TEST 完成，误差 %1 ‰").arg(err));
        if (err == 0)
            log("  提示：0‰ 可能是频率整除无误差，也可能是 PA1→PA6 跳线未接");
    });
}

void MainWindow::doRefreshStatus()
{
    if (!m_dev->isConnected())
        return;
    guarded("读取状态", [&] {
        const Status st = m_dev->getStatus();
        m_runValue->setText(st.running ? "运行中" : "已停止");
        m_freqValue->setText(QString::number(st.freq_hz) + " Hz");
        // 不写 m_errValue：它只由 SELF_TEST 更新，轮询不能覆盖。
        refreshErrLabel();
        refreshValueStyles();
    });
}

void MainWindow::onPollTimer()
{
    // 后台轮询：失败时静默，避免每秒弹窗打断操作
    if (!m_dev->isConnected())
        return;
    try {
        const Status st = m_dev->getStatus();
        m_runValue->setText(st.running ? "运行中" : "已停止");
        m_freqValue->setText(QString::number(st.freq_hz) + " Hz");
        // GET_STATUS.err 是固件预留位（恒 0），轮询不刷新实测误差标签。
        // GET_STATUS.err is reserved by firmware (always 0); don't touch label.
        refreshValueStyles();
    } catch (const std::exception&) {
        // ignore
    }
}
