#ifndef JSONRPCWEBSOCKETCLIENT_H
#define JSONRPCWEBSOCKETCLIENT_H

#include <QAbstractSocket>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QHash>
#include <QUrl>
#include <functional>

class QWebSocket;
class QTimer;

class JsonRpcWebSocketClient : public QObject
{
    Q_OBJECT
public:
    enum class ConnectionState {
        Unknown,
        Connecting,
        Ready,
        Retrying,
        Unavailable,
        ProtocolError,
        Disconnected
    };
    Q_ENUM(ConnectionState)

    enum class FailureCategory {
        None,
        ServiceUnavailable,
        PortConflictOrProtocol,
        TemporarilyUnavailable,
        ProtocolError,
        Unknown
    };
    Q_ENUM(FailureCategory)

    explicit JsonRpcWebSocketClient(QObject *parent = nullptr);
    ~JsonRpcWebSocketClient();

    void connectToServer(const QUrl &url);
    void retryNow();
    void stop();
    bool isConnected() const;

    ConnectionState connectionState() const;
    QString lastError() const;
    QAbstractSocket::SocketError socketError() const;
    QDateTime lastConnectedAt() const;
    int retryCount() const;
    FailureCategory failureCategory() const;
    QString diagnosticReport() const;
    static QString connectionStateName(ConnectionState state);
    static QString failureCategoryName(FailureCategory category);

    void callAsync(const QString &method, const int id, const QJsonObject &args,
                   std::function<void(QJsonValue)> callback = nullptr);
    void callAsync(const QString &method, const int id,
                   std::function<void(QJsonValue)> callback = nullptr);

    void registerCallback(const int id, std::function<void(QJsonValue)> callback);

signals:
    void error(const QString &error) const;
    void connectionError(QAbstractSocket::SocketError socketError,
                         JsonRpcWebSocketClient::FailureCategory category,
                         const QString &error,
                         int retryCount,
                         bool willRetry);
    void stateChanged(JsonRpcWebSocketClient::ConnectionState state);
    void connected();
    void disconnected();
private slots:
    void onTextMessageReceived(const QString &message);
    void onSocketError(QAbstractSocket::SocketError socketError);
    void onSocketDisconnected();
    void onRetryTimeout();
    void onConnectionTimeout();
private:
    static constexpr int ConnectionTimeoutMs = 3000;
    static constexpr int MaxRetryCount = 5;

    void openCurrentUrl();
    void handleConnectionFailure(QAbstractSocket::SocketError socketError,
                                 const QString &error);
    void setConnectionState(ConnectionState state);
    static int retryDelayMs(int retryCount);
    static FailureCategory categoryForSocketError(QAbstractSocket::SocketError socketError);

    bool m_connected = false;
    bool m_shouldReconnect = false;
    bool m_connectionAttemptActive = false;
    bool m_handlingFailure = false;
    bool m_wasConnected = false;
    ConnectionState m_connectionState = ConnectionState::Unknown;
    QUrl m_url;
    QString m_lastError;
    QAbstractSocket::SocketError m_socketError = QAbstractSocket::UnknownSocketError;
    FailureCategory m_failureCategory = FailureCategory::None;
    QDateTime m_lastConnectedAt;
    int m_retryCount = 0;
    QHash<int, std::function<void(QJsonValue)>> m_callbacks;

    QWebSocket *webSocket = nullptr;
    QTimer *m_retryTimer = nullptr;
    QTimer *m_connectionTimer = nullptr;
};

#endif // JSONRPCWEBSOCKETCLIENT_H
