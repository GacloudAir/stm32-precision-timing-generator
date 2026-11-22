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
    resize(560, 620);
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
    setStyleSheet(R"(
        QGroupBox { font-weight: 500; border: 1px solid #d8d8d8; border-radius: 8px;
                    margin-top: 10px; padding: 10px 10px 8px 10px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #555; }
        QPushButton { padding: 6px 16px; border: 1px solid #c8c8c8; border-radius: 6px;
                      background: #fafafa; }
        QPushButton:hover { background: #f0f0f0; }
        QPushButton:pressed { background: #e8e8e8; }
        QPushButton:disabled { color: #b0b0b0; background: #f7f7f7; }
        QLineEdit, QComboBox { padding: 5px 8px; border: 1px solid #c8c8c8; border-radius: 6px;
                               background: #fff; }
        QPlainTextEdit { border: 1px solid #e6e6e6; border-radius: 6px; background: #fcfcfc; }
    )");

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(14, 10, 14, 14);
    root->setSpacing(4);

    // ---------------- 连接 ----------------
    auto* connBox = new QGroupBox("连接", central);
    auto* connLay = new QHBoxLayout(connBox);
    m_portBox = new QComboBox(connBox);
    m_portBox->setMinimumWidth(230);
    auto* refreshBtn = new QPushButton("刷新", connBox);
    m_connBtn = new QPushButton("连接", connBox);
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
    m_selfTestBtn = new QPushButton("自检 (SELF_TEST)", ctrlBox);
    freqRow->addWidget(new QLabel("频率(Hz):", ctrlBox));
    freqRow->addWidget(m_freqEdit);
    freqRow->addWidget(m_setBtn);
    freqRow->addStretch(1);
    freqRow->addWidget(m_selfTestBtn);
    ctrlLay->addLayout(freqRow);

    auto* runRow = new QHBoxLayout();
    m_startBtn = new QPushButton("启动输出", ctrlBox);
    m_stopBtn = new QPushButton("停止输出", ctrlBox);
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
    m_connLabel->setStyleSheet(connected ? "color:#1D9E75; font-weight:500;"
                                         : "color:#A32D2D; font-weight:500;");
    m_connBtn->setText(connected ? "断开" : "连接");
    m_portBox->setEnabled(!connected);
    m_setBtn->setEnabled(connected);
    m_startBtn->setEnabled(connected);
    m_stopBtn->setEnabled(connected);
    m_selfTestBtn->setEnabled(connected);
}

void MainWindow::toggleConnection()
{
    if (m_dev->isConnected()) {
        m_dev->disconnectPort();
        setConnectedUi(false);
        m_runValue->setText("--");
        m_freqValue->setText("--");
        m_errValue->setText("--");
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
        m_errValue->setText(QString::number(err) + " ‰");
        log(QString("SELF_TEST 完成，误差 %1 ‰").arg(err));
        if (err == 0)
            log("  提示：0‰ 也可能是 PA1→PA6 跳线未接（捕获超时）");
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
        m_errValue->setText(QString::number(st.err_permille) + " ‰");
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
        m_errValue->setText(QString::number(st.err_permille) + " ‰");
    } catch (const std::exception&) {
        // ignore
    }
}
