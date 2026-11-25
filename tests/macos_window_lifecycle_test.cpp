#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTcpServer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

#include "anylink.h"
#include "common.h"
#include "configmanager.h"
#include "detaildialog.h"
#include "jsonrpcwebsocketclient.h"

class MacOSWindowLifecycleTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        configLocation = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QVERIFY(QDir().mkpath(configLocation));
        tempLocation = QDir::tempPath();

        configManager = new ConfigManager;
        QVERIFY(configManager->loadConfig(Json));
        configManager->config["autoLogin"] = false;
        configManager->config["minimize"] = false;
    }

    void cleanupTestCase()
    {
        delete configManager;
        configManager = nullptr;
    }

    void restoresHiddenAndMinimizedWindow()
    {
        AnyLink window;
        window.showMainWindow();
        QVERIFY(window.isVisible());

        window.showMinimized();
        QCoreApplication::processEvents();
        QVERIFY(window.windowState() & Qt::WindowMinimized);

        window.showMainWindow();
        QCoreApplication::processEvents();
        QVERIFY(window.isVisible());
        QVERIFY(!(window.windowState() & Qt::WindowMinimized));

        QSignalSpy quitSpy(qApp, &QCoreApplication::aboutToQuit);
        window.close();
        QCoreApplication::processEvents();
        QVERIFY(!window.isVisible());
        QCOMPARE(quitSpy.count(), 0);

        window.showMainWindow();
        QVERIFY(window.isVisible());
    }

    void repeatedShowDoesNotDuplicateInitialization()
    {
        AnyLink window;
        window.showMainWindow();
        QVERIFY(window.rpc != nullptr);

        const int actionCount = window.findChildren<QAction *>().size();
        window.showMainWindow();
        window.showMainWindow();

        QCOMPARE(window.findChildren<QAction *>().size(), actionCount);
        QVERIFY(window.rpc != nullptr);
    }

    void agentConnectionReachesReady()
    {
        QWebSocketServer server(QStringLiteral("AnyLink test agent"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        QWebSocket *peer = nullptr;
        connect(&server, &QWebSocketServer::newConnection, this, [&]() {
            peer = server.nextPendingConnection();
        });

        JsonRpcWebSocketClient client;
        QSignalSpy connectedSpy(&client, &JsonRpcWebSocketClient::connected);
        const QUrl endpoint(QStringLiteral("ws://127.0.0.1:%1/rpc")
                                .arg(server.serverPort()));
        client.connectToServer(endpoint);

        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);
        QCOMPARE(client.connectionState(), JsonRpcWebSocketClient::ConnectionState::Ready);
        QCOMPARE(client.retryCount(), 0);
        QVERIFY(client.lastConnectedAt().isValid());
        QVERIFY(client.diagnosticReport().contains(QStringLiteral("127.0.0.1")));

        client.stop();
        server.close();
        delete peer;
    }

    void agentConnectionRetriesWithRawError()
    {
        QTcpServer portPicker;
        QVERIFY(portPicker.listen(QHostAddress::LocalHost, 0));
        const quint16 unusedPort = portPicker.serverPort();
        portPicker.close();

        JsonRpcWebSocketClient client;
        client.connectToServer(QUrl(QStringLiteral("ws://127.0.0.1:%1/rpc")
                                        .arg(unusedPort)));

        QTRY_VERIFY_WITH_TIMEOUT(client.connectionState()
                                     == JsonRpcWebSocketClient::ConnectionState::Retrying,
                                 2000);
        QCOMPARE(client.retryCount(), 1);
        QVERIFY(client.failureCategory() == JsonRpcWebSocketClient::FailureCategory::ServiceUnavailable
                || client.failureCategory()
                       == JsonRpcWebSocketClient::FailureCategory::PortConflictOrProtocol);
        QVERIFY(!client.lastError().isEmpty());
        QVERIFY(client.diagnosticReport().contains(client.lastError()));

        client.stop();
    }

    void noDnsConfigIsPersistedAndSentOverRpc()
    {
        configManager->config["no_dns"] = false;

        QWebSocketServer server(QStringLiteral("AnyLink config test agent"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        QList<QJsonObject> receivedConfigs;
        QWebSocket *peer = nullptr;
        connect(&server, &QWebSocketServer::newConnection, this, [&]() {
            peer = server.nextPendingConnection();
            connect(peer, &QWebSocket::binaryMessageReceived, this, [&](const QByteArray &msg) {
                const QJsonObject req = QJsonDocument::fromJson(msg).object();
                if (req.value(QStringLiteral("method")).toString() == QStringLiteral("config")) {
                    receivedConfigs.append(req.value(QStringLiteral("params")).toObject());
                    const QJsonObject reply{
                        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), req.value(QStringLiteral("id"))},
                        {QStringLiteral("result"), QStringLiteral("ready to connect")},
                    };
                    peer->sendTextMessage(QString::fromUtf8(
                        QJsonDocument(reply).toJson(QJsonDocument::Compact)));
                }
            });
        });

        AnyLink window;
        window.showMainWindow();
        QVERIFY(window.rpc != nullptr);

        auto *checkBoxNoDns = window.findChild<QCheckBox *>(QStringLiteral("checkBoxNoDns"));
        QVERIFY(checkBoxNoDns != nullptr);
        QCOMPARE(checkBoxNoDns->isChecked(), false);

        window.rpc->stop();
        window.rpc->connectToServer(
            QUrl(QStringLiteral("ws://127.0.0.1:%1/rpc").arg(server.serverPort())));
        QTRY_VERIFY_WITH_TIMEOUT(receivedConfigs.size() >= 1, 2000);
        QCOMPARE(receivedConfigs.last().value(QStringLiteral("no_dns")).toBool(), false);

        checkBoxNoDns->setChecked(true);
        QCOMPARE(configManager->config["no_dns"].toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(receivedConfigs.size() >= 2, 2000);
        QCOMPARE(receivedConfigs.last().value(QStringLiteral("no_dns")).toBool(), true);

        checkBoxNoDns->setChecked(false);
        QCOMPARE(configManager->config["no_dns"].toBool(), false);
        QTRY_VERIFY_WITH_TIMEOUT(receivedConfigs.size() >= 3, 2000);
        QCOMPARE(receivedConfigs.last().value(QStringLiteral("no_dns")).toBool(), false);

        window.rpc->stop();
        server.close();
        delete peer;
    }

    void upstreamDnsPanelDisplaysAndCopiesWhenGlobalDnsDisabled()
    {
        configManager->config["no_dns"] = true;

        AnyLink window;
        window.showMainWindow();

        const QJsonObject status{
            {QStringLiteral("DtlsConnected"), false},
            {QStringLiteral("TLSCipherSuite"), QStringLiteral("TLS_AES_128_GCM_SHA256")},
            {QStringLiteral("ServerAddress"), QStringLiteral("203.0.113.10")},
            {QStringLiteral("LocalAddress"), QStringLiteral("192.168.0.10")},
            {QStringLiteral("VPNAddress"), QStringLiteral("10.20.0.2")},
            {QStringLiteral("MTU"), 1399},
            {QStringLiteral("DNS"), QJsonArray{QStringLiteral("10.10.0.53"), QStringLiteral("10.10.0.54")}},
            {QStringLiteral("DNSApplied"), false},
            {QStringLiteral("DefaultDomain"), QStringLiteral("corp.internal")},
            {QStringLiteral("SplitDNS"), QJsonArray{QStringLiteral("dev.corp.internal")}},
            {QStringLiteral("SplitInclude"), QJsonArray{QStringLiteral("10.10.0.0/255.255.0.0")}},
            {QStringLiteral("SplitExclude"), QJsonArray{}},
        };

        window.applyVpnStatus(status);

        auto *labelDNS = window.findChild<QLabel *>(QStringLiteral("labelDNS"));
        auto *buttonCopyDns = window.findChild<QPushButton *>(QStringLiteral("buttonCopyDns"));
        QVERIFY(labelDNS != nullptr);
        QVERIFY(buttonCopyDns != nullptr);
        QVERIFY(labelDNS->text().contains(QStringLiteral("10.10.0.53, 10.10.0.54")));
        QVERIFY(labelDNS->text().contains(QStringLiteral("Not applied")));
        QVERIFY(buttonCopyDns->isEnabled());

        DetailDialog *details = window.detailsDialog();
        QVERIFY(details != nullptr);
        auto *tableDns = details->findChild<QTableWidget *>(QStringLiteral("tableDns"));
        auto *labelDnsState = details->findChild<QLabel *>(QStringLiteral("labelDnsState"));
        QVERIFY(tableDns != nullptr);
        QVERIFY(labelDnsState != nullptr);

        QCOMPARE(tableDns->rowCount(), 4);
        QCOMPARE(tableDns->item(0, 1)->text(), QStringLiteral("10.10.0.53"));
        QCOMPARE(tableDns->item(1, 1)->text(), QStringLiteral("10.10.0.54"));
        QCOMPARE(tableDns->item(2, 1)->text(), QStringLiteral("corp.internal"));
        QCOMPARE(tableDns->item(3, 1)->text(), QStringLiteral("dev.corp.internal"));
        QVERIFY(labelDnsState->text().contains(QStringLiteral("Not applied to system")));

        tableDns->selectRow(1);
        QCOMPARE(details->copySelectedDnsToClipboard(), QStringLiteral("10.10.0.54"));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("10.10.0.54"));

        buttonCopyDns->click();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("10.10.0.53, 10.10.0.54"));

        configManager->config["no_dns"] = false;
    }

    void outdatedAgentAndEmptySplitIncludeShowWarnings()
    {
        configManager->config["no_dns"] = true;

        AnyLink window;
        window.showMainWindow();

        // Case 1: older vpnagent does not include DNSApplied in status response
        const QJsonObject legacyStatus{
            {QStringLiteral("DNS"), QJsonArray{QStringLiteral("10.10.0.53")}},
            {QStringLiteral("SplitInclude"), QJsonArray{QStringLiteral("10.10.0.0/255.255.0.0")}},
        };
        window.applyVpnStatus(legacyStatus);

        auto *labelDNS = window.findChild<QLabel *>(QStringLiteral("labelDNS"));
        DetailDialog *details = window.detailsDialog();
        auto *labelDnsState = details->findChild<QLabel *>(QStringLiteral("labelDnsState"));
        QVERIFY(labelDNS->text().contains(QStringLiteral("update vpnagent")));
        QVERIFY(labelDnsState->text().contains(QStringLiteral("DNSApplied")));

        // Case 2: new vpnagent skips DNS, but gateway pushed no SplitInclude routes
        const QJsonObject globalModeStatus{
            {QStringLiteral("DNS"), QJsonArray{QStringLiteral("10.10.0.53")}},
            {QStringLiteral("DNSApplied"), false},
            {QStringLiteral("SplitInclude"), QJsonArray{}},
        };
        window.applyVpnStatus(globalModeStatus);
        QVERIFY(labelDnsState->text().contains(QStringLiteral("default route was not changed")));

        configManager->config["no_dns"] = false;
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    MacOSWindowLifecycleTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "macos_window_lifecycle_test.moc"
