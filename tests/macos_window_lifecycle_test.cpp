#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QHostAddress>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

#include "anylink.h"
#include "common.h"
#include "configmanager.h"
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
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    MacOSWindowLifecycleTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "macos_window_lifecycle_test.moc"
