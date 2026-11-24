#include "jsonrpcwebsocketclient.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <QWebSocket>

JsonRpcWebSocketClient::JsonRpcWebSocketClient(QObject *parent)
    : QObject(parent)
    , webSocket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , m_retryTimer(new QTimer(this))
    , m_connectionTimer(new QTimer(this))
{
    m_retryTimer->setSingleShot(true);
    m_connectionTimer->setSingleShot(true);

    connect(webSocket, &QWebSocket::connected, this, [this]() {
        m_connected = true;
        m_wasConnected = true;
        m_connectionAttemptActive = false;
        m_retryCount = 0;
        m_lastError.clear();
        m_socketError = QAbstractSocket::UnknownSocketError;
        m_failureCategory = FailureCategory::None;
        m_lastConnectedAt = QDateTime::currentDateTimeUtc();
        m_retryTimer->stop();
        m_connectionTimer->stop();
        setConnectionState(ConnectionState::Ready);
        emit connected();
    });
    connect(webSocket, &QWebSocket::disconnected,
            this, &JsonRpcWebSocketClient::onSocketDisconnected);
    connect(webSocket, &QWebSocket::errorOccurred,
            this, &JsonRpcWebSocketClient::onSocketError);
    connect(webSocket, &QWebSocket::textMessageReceived,
            this, &JsonRpcWebSocketClient::onTextMessageReceived);
    connect(m_retryTimer, &QTimer::timeout,
            this, &JsonRpcWebSocketClient::onRetryTimeout);
    connect(m_connectionTimer, &QTimer::timeout,
            this, &JsonRpcWebSocketClient::onConnectionTimeout);
}

JsonRpcWebSocketClient::~JsonRpcWebSocketClient()
{
    stop();
}

void JsonRpcWebSocketClient::connectToServer(const QUrl &url)
{
    m_url = url;
    m_shouldReconnect = true;
    m_retryCount = 0;
    m_lastError.clear();
    m_socketError = QAbstractSocket::UnknownSocketError;
    m_failureCategory = FailureCategory::None;
    m_retryTimer->stop();

    if (!m_url.isValid() || (m_url.scheme() != QLatin1String("ws")
                             && m_url.scheme() != QLatin1String("wss"))) {
        m_shouldReconnect = false;
        handleConnectionFailure(QAbstractSocket::UnknownSocketError,
                                tr("Invalid WebSocket endpoint: %1").arg(m_url.toString()));
        return;
    }

    openCurrentUrl();
}

void JsonRpcWebSocketClient::retryNow()
{
    if (m_url.isEmpty() || m_connectionState == ConnectionState::Ready) {
        return;
    }

    m_shouldReconnect = true;
    m_retryCount = 0;
    m_lastError.clear();
    m_socketError = QAbstractSocket::UnknownSocketError;
    m_failureCategory = FailureCategory::None;
    m_retryTimer->stop();
    openCurrentUrl();
}

void JsonRpcWebSocketClient::stop()
{
    const bool wasConnected = m_connected || m_wasConnected;

    m_shouldReconnect = false;
    m_connectionAttemptActive = false;
    m_retryTimer->stop();
    m_connectionTimer->stop();
    m_callbacks.clear();

    m_handlingFailure = true;
    if (webSocket != nullptr && webSocket->state() != QAbstractSocket::UnconnectedState) {
        webSocket->abort();
    }
    m_handlingFailure = false;

    m_connected = false;
    m_wasConnected = false;
    if (wasConnected) {
        emit disconnected();
    }
    setConnectionState(ConnectionState::Disconnected);
}

bool JsonRpcWebSocketClient::isConnected() const
{
    return m_connected;
}

JsonRpcWebSocketClient::ConnectionState JsonRpcWebSocketClient::connectionState() const
{
    return m_connectionState;
}

QString JsonRpcWebSocketClient::lastError() const
{
    return m_lastError;
}

QAbstractSocket::SocketError JsonRpcWebSocketClient::socketError() const
{
    return m_socketError;
}

QDateTime JsonRpcWebSocketClient::lastConnectedAt() const
{
    return m_lastConnectedAt;
}

int JsonRpcWebSocketClient::retryCount() const
{
    return m_retryCount;
}

JsonRpcWebSocketClient::FailureCategory JsonRpcWebSocketClient::failureCategory() const
{
    return m_failureCategory;
}

QString JsonRpcWebSocketClient::diagnosticReport() const
{
    QString report;
    QTextStream stream(&report);
    stream << "AnyLink local agent diagnostics\n";
    stream << "endpoint: " << m_url.toString() << '\n';
    stream << "state: " << connectionStateName(m_connectionState) << '\n';
    stream << "socket error: " << static_cast<int>(m_socketError) << '\n';
    stream << "failure category: " << failureCategoryName(m_failureCategory) << '\n';
    stream << "retry count: " << m_retryCount << '\n';
    stream << "last connected: "
           << (m_lastConnectedAt.isValid() ? m_lastConnectedAt.toString(Qt::ISODate)
                                            : QStringLiteral("never"))
           << '\n';
    stream << "last error: " << (m_lastError.isEmpty() ? QStringLiteral("none") : m_lastError)
           << '\n';
    stream << "service: sslcon (vpnagent executable), TCP 127.0.0.1:6210\n";
    return report;
}

QString JsonRpcWebSocketClient::connectionStateName(ConnectionState state)
{
    switch (state) {
    case ConnectionState::Unknown:
        return QStringLiteral("unknown");
    case ConnectionState::Connecting:
        return QStringLiteral("connecting");
    case ConnectionState::Ready:
        return QStringLiteral("ready");
    case ConnectionState::Retrying:
        return QStringLiteral("retrying");
    case ConnectionState::Unavailable:
        return QStringLiteral("unavailable");
    case ConnectionState::ProtocolError:
        return QStringLiteral("protocol-error");
    case ConnectionState::Disconnected:
        return QStringLiteral("disconnected");
    }
    return QStringLiteral("unknown");
}

QString JsonRpcWebSocketClient::failureCategoryName(FailureCategory category)
{
    switch (category) {
    case FailureCategory::None:
        return QStringLiteral("none");
    case FailureCategory::ServiceUnavailable:
        return QStringLiteral("service-unavailable");
    case FailureCategory::PortConflictOrProtocol:
        return QStringLiteral("port-conflict-or-protocol");
    case FailureCategory::TemporarilyUnavailable:
        return QStringLiteral("temporarily-unavailable");
    case FailureCategory::ProtocolError:
        return QStringLiteral("protocol-error");
    case FailureCategory::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

void JsonRpcWebSocketClient::callAsync(const QString &method, const int id,
                                       const QJsonObject &args,
                                       std::function<void(QJsonValue)> callback)
{
    QJsonObject jsonRpc {
        {"method", method},
        {"jsonrpc", "2.0"},
        {"params", args},
        {"id", id},
    };

    if (!m_connected || webSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    if (!m_callbacks.contains(id) && callback != nullptr) {
        m_callbacks.insert(id, callback);
    }
    webSocket->sendBinaryMessage(QJsonDocument(jsonRpc).toJson(QJsonDocument::Compact));
}

void JsonRpcWebSocketClient::callAsync(const QString &method, const int id,
                                       std::function<void(QJsonValue)> callback)
{
    callAsync(method, id, QJsonObject(), callback);
}

/**
 * @brief Do not send anything; just wait for the server to call.
 */
void JsonRpcWebSocketClient::registerCallback(const int id,
                                              std::function<void(QJsonValue)> callback)
{
    if (!m_callbacks.contains(id)) {
        m_callbacks.insert(id, callback);
    }
}

void JsonRpcWebSocketClient::onTextMessageReceived(const QString &message)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        const QString error = tr("Invalid JSON-RPC response: %1").arg(parseError.errorString());
        m_lastError = error;
        m_failureCategory = FailureCategory::ProtocolError;
        setConnectionState(ConnectionState::ProtocolError);
        emit this->error(error);
        emit connectionError(QAbstractSocket::UnknownSocketError,
                             m_failureCategory,
                             error,
                             m_retryCount,
                             false);
        return;
    }

    const QJsonObject result = doc.object();
    if (m_connectionState == ConnectionState::ProtocolError) {
        m_failureCategory = FailureCategory::None;
        setConnectionState(ConnectionState::Ready);
    }
    if (!result.contains("id")) {
        const QString error = tr("JSON-RPC response does not contain an id");
        m_lastError = error;
        m_failureCategory = FailureCategory::ProtocolError;
        setConnectionState(ConnectionState::ProtocolError);
        emit this->error(error);
        emit connectionError(QAbstractSocket::UnknownSocketError,
                             m_failureCategory,
                             error,
                             m_retryCount,
                             false);
        return;
    }

    const int id = result.value("id").toInt();
    if (m_callbacks.contains(id)) {
        const auto callback = m_callbacks.take(id);
        if (result.contains("error")) {
            callback(result.value("error"));
        } else {
            callback(result.value("result"));
        }
    }
}

void JsonRpcWebSocketClient::onSocketError(QAbstractSocket::SocketError socketError)
{
    if (m_handlingFailure || !m_shouldReconnect
        || (!m_connectionAttemptActive && m_connectionState != ConnectionState::Ready)) {
        return;
    }

    handleConnectionFailure(socketError, webSocket->errorString());
}

void JsonRpcWebSocketClient::onSocketDisconnected()
{
    if (m_handlingFailure || !m_shouldReconnect) {
        return;
    }

    if (m_connectionState == ConnectionState::Ready || m_connectionAttemptActive) {
        handleConnectionFailure(QAbstractSocket::RemoteHostClosedError,
                                tr("The local agent closed the WebSocket connection"));
    }
}

void JsonRpcWebSocketClient::onRetryTimeout()
{
    if (m_shouldReconnect && m_connectionState != ConnectionState::Ready) {
        openCurrentUrl();
    }
}

void JsonRpcWebSocketClient::onConnectionTimeout()
{
    if (m_connectionAttemptActive && m_connectionState == ConnectionState::Connecting) {
        handleConnectionFailure(QAbstractSocket::SocketTimeoutError,
                                tr("Timed out waiting for the local agent"));
    }
}

void JsonRpcWebSocketClient::openCurrentUrl()
{
    if (!m_shouldReconnect || m_url.isEmpty()) {
        return;
    }

    m_connectionAttemptActive = true;
    setConnectionState(ConnectionState::Connecting);

    if (webSocket->state() != QAbstractSocket::UnconnectedState) {
        m_handlingFailure = true;
        webSocket->abort();
        m_handlingFailure = false;
    }

    m_connectionTimer->start(ConnectionTimeoutMs);
    webSocket->open(m_url);
}

void JsonRpcWebSocketClient::handleConnectionFailure(QAbstractSocket::SocketError socketError,
                                                      const QString &error)
{
    if (m_handlingFailure) {
        return;
    }

    m_handlingFailure = true;
    const bool wasConnected = m_connected || m_wasConnected;
    m_connected = false;
    m_wasConnected = false;
    m_connectionAttemptActive = false;
    m_connectionTimer->stop();
    m_callbacks.clear();

    m_socketError = socketError;
        m_lastError = error.trimmed();
    if (m_lastError.isEmpty()) {
        m_lastError = tr("The local agent connection failed (socket error %1)")
                          .arg(static_cast<int>(socketError));
    }

    if (webSocket->state() != QAbstractSocket::UnconnectedState) {
        webSocket->abort();
    }
    m_handlingFailure = false;

    if (wasConnected) {
        emit disconnected();
    }

    m_failureCategory = categoryForSocketError(socketError);
    const bool willRetry = m_shouldReconnect && m_url.isValid() && m_retryCount < MaxRetryCount;
    if (willRetry) {
        ++m_retryCount;
        setConnectionState(ConnectionState::Retrying);
        m_retryTimer->start(retryDelayMs(m_retryCount));
    } else {
        setConnectionState(ConnectionState::Unavailable);
    }

    emit this->error(m_lastError);
    emit connectionError(m_socketError,
                         m_failureCategory,
                         m_lastError,
                         m_retryCount,
                         willRetry);
}

void JsonRpcWebSocketClient::setConnectionState(ConnectionState state)
{
    if (m_connectionState == state) {
        return;
    }
    m_connectionState = state;
    emit stateChanged(state);
}

int JsonRpcWebSocketClient::retryDelayMs(int retryCount)
{
    const int exponent = qBound(0, retryCount - 1, 4);
    return 500 * (1 << exponent);
}

JsonRpcWebSocketClient::FailureCategory
JsonRpcWebSocketClient::categoryForSocketError(QAbstractSocket::SocketError socketError)
{
    switch (socketError) {
    case QAbstractSocket::ConnectionRefusedError:
    case QAbstractSocket::HostNotFoundError:
    case QAbstractSocket::NetworkError:
        return FailureCategory::ServiceUnavailable;
    case QAbstractSocket::RemoteHostClosedError:
        return FailureCategory::PortConflictOrProtocol;
    case QAbstractSocket::SocketTimeoutError:
        return FailureCategory::TemporarilyUnavailable;
    case QAbstractSocket::UnknownSocketError:
        return FailureCategory::Unknown;
    default:
        return FailureCategory::ServiceUnavailable;
    }
}
