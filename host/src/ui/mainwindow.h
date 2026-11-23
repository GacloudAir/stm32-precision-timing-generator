#pragma once

#include <QMainWindow>
#include <functional>

class QComboBox;
class QPushButton;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QTimer;
class TimingDevice;

// 主窗口：只通过 TimingDevice 的 8 个方法与下位机交互。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void refreshPorts();
    void toggleConnection();
    void applyFrequency();
    void doStart();
    void doStop();
    void doSelfTest();
    void doRefreshStatus();
    void onPollTimer();

private:
    void buildUi();
    void log(const QString& msg);
    void setConnectedUi(bool connected);
    // 只显示 SELF_TEST 的实测误差；GET_STATUS 的 err 字段是固件预留位（恒 0）。
    // Show only the SELF_TEST measurement; GET_STATUS.err is reserved by the
    // firmware and always reads 0, so polling must not overwrite this label.
    void refreshErrLabel();
    // 统一把 DeviceError 转成界面提示，避免每个槽都写 try/catch 重复代码
    void guarded(const QString& what, const std::function<void()>& fn);

    TimingDevice*   m_dev        = nullptr;
    QComboBox*      m_portBox    = nullptr;
    QPushButton*    m_connBtn    = nullptr;
    QLabel*         m_connLabel  = nullptr;
    QLineEdit*      m_freqEdit   = nullptr;
    QPushButton*    m_setBtn     = nullptr;
    QPushButton*    m_startBtn   = nullptr;
    QPushButton*    m_stopBtn    = nullptr;
    QPushButton*    m_selfTestBtn= nullptr;
    QLabel*         m_runValue   = nullptr;
    QLabel*         m_freqValue  = nullptr;
    QLabel*         m_errValue   = nullptr;
    QPlainTextEdit* m_log        = nullptr;
    QTimer*         m_pollTimer  = nullptr;
    int             m_lastSelfTestErr = -1;   // -1 = 尚未自检 / not measured yet
};
