#ifndef ANYLINK_H
#define ANYLINK_H

#include <QWidget>
#include <QIcon>
#include <QJsonObject>
#include <QTimer>

QT_BEGIN_NAMESPACE
namespace Ui {
class AnyLink;
}
QT_END_NAMESPACE

class JsonRpcWebSocketClient;
class ProfileManager;
class DetailDialog;
class QSystemTrayIcon;
class QMenu;
class QComboBox;

class AnyLink : public QWidget
{
    Q_OBJECT

public:
    enum { STATUS, CONFIG, CONNECT, DISCONNECT, RECONNECT, INTERFACE, ABORT, STAT };

    AnyLink(QWidget *parent = nullptr);
    ~AnyLink();

    JsonRpcWebSocketClient *rpc = nullptr;

public slots:
    void showMainWindow();

signals:
    void vpnConnected();
    void vpnClosed();

    // QWidget interface
protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    Ui::AnyLink *ui;
    QSystemTrayIcon *trayIcon = nullptr;
    QMenu *trayIconMenu = nullptr;
    QComboBox *iconComboBox = nullptr;
    QAction *actionConnect = nullptr;
    QAction *actionDisconnect = nullptr;
    QAction *actionQuit = nullptr;
    QAction *actionConfig = nullptr;
    QAction *actionRetryAgent = nullptr;
    QAction *actionCopyDiagnostics = nullptr;
    QTimer timer;

    QIcon iconConnected = QIcon(":/assets/connected.png");
    QIcon iconNotConnected = QIcon(":/assets/notconnected.png");
    QIcon iconConnecting = QIcon(":/assets/connecting.png");

    ProfileManager *profileManager = nullptr;
    bool m_vpnConnected = false;
    bool m_autoLoginRequested = false;
    bool m_initialized = false;
    bool m_initializationScheduled = false;
    bool m_quitRequested = false;
    bool activeDisconnect = false;
    DetailDialog *detailDialog = nullptr;
    QJsonObject currentProfile = {};

    void center();
    void loadStyleSheet(const QString &styleSheetFile);

    void createTrayActions();
    void createTrayIcon();
    void initConfig();
    void afterShowOneTime();
    void ensureInitialized();
    void hideMainWindow();
    void quitApplication();

    void resetVPNStatus();

    void saveConfig();

private slots:
    void configVPN();
    void connectVPN(bool reconnect = false);
    void disconnectVPN();
    void getVPNStatus();

    //  QMetaObject::connectSlotsByName
    void on_buttonConnect_clicked();
    void on_buttonProfile_clicked();
    void on_buttonViewLog_clicked();
    void on_buttonDetails_clicked();
    void on_buttonSecurityTips_clicked();
    void on_buttonAgentCheck_clicked();
    void on_buttonCopyDiagnostics_clicked();
};
#endif // ANYLINK_H
