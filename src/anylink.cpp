#include "anylink.h"
#include <QCloseEvent>
#include <QFile>
#include <QJsonValue>
#include <QtWidgets>
#include "configmanager.h"
#include "detaildialog.h"
#include "jsonrpcwebsocketclient.h"
#include "profilemanager.h"
#include "textbrowser.h"
#include "ui_anylink.h"

#if defined(Q_OS_MACOS)
#include "macdockiconhandler.h"
#endif

AnyLink::AnyLink(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::AnyLink)
{
    ui->setupUi(this);
#ifndef Q_OS_MACOS
    layout()->removeItem(ui->topSpacer);
    setWindowFlags(Qt::Dialog);
#else
    setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowMinimizeButtonHint
                   | Qt::WindowCloseButtonHint);
#endif
    setWindowTitle(tr("AnyLink Secure Client") + " v" + appVersion);

#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    loadStyleSheet(":/resource/style.qss");
    setWindowIcon(QIcon(":/images/anylink64.png"));
#endif
    // qDebug() << screen()->devicePixelRatio() << geometry().width() << geometry().height() << QSysInfo::kernelType();
    // 需要联合使用 QSysInfo::kernelType() 和  QSysInfo::productType()
    const QString kernelType = QSysInfo::kernelType();
    if (screen()->devicePixelRatio() > 1.0 && (kernelType == "linux" || kernelType == "darwin")) {
        setFixedSize(geometry().width(), geometry().height());
        // setFixedSize(560, 390);
    } else {
        setFixedSize(490, 330);
    }

    center();
    ui->lineEditOTP->setFocus();
    connect(ui->lineEditOTP, &QLineEdit::returnPressed, this, [this]() {
        if (!ui->lineEditOTP->text().isEmpty()) {
            if (rpc->isConnected()) {
                connectVPN();
            }
        }
    });

    profileManager = new ProfileManager(this);

    if(profileManager->loadProfile(Json)) {
        profileManager->updateModel();
        ui->comboBoxHost->setModel(profileManager->model);
        // update by vpnConnected
        QString lastProfile = configManager->config["lastProfile"].toString();
        if(!lastProfile.isEmpty() && profileManager->model->stringList().contains(lastProfile)) {
            ui->comboBoxHost->setCurrentText(lastProfile);
        } else {
            ui->comboBoxHost->setCurrentIndex(0);
        }

        // 每个 profile 的密码被读取后都会发送 keyRestored
        connect(profileManager, &ProfileManager::keyRestored, this, [this](const QString &profile) {
            // keychain 中的密码是异步获取的
            QTimer::singleShot(500, this, [this, profile]() {
                if (configManager->config["autoLogin"].toBool()) {
                    const QString lastProfile = configManager->config["lastProfile"].toString();
                    if (lastProfile == profile) {
                        m_autoLoginRequested = true;
                        connectVPN();
                    }
                }
            });
        });
    }
    // exit
}

AnyLink::~AnyLink()
{
    // Closing the socket can synchronously emit an error; keep callbacks from
    // reaching the UI while the object is being torn down.
    if (rpc != nullptr) {
        disconnect(rpc, nullptr, this, nullptr);
        delete rpc;
        rpc = nullptr;
    }

    delete ui;
}

void AnyLink::closeEvent(QCloseEvent *event)
{
    if (m_quitRequested) {
        event->accept();
        return;
    }

    hideMainWindow();
    event->accept();
}

void AnyLink::showEvent(QShowEvent *event)
{
    if (!m_initialized && !m_initializationScheduled) {
        m_initializationScheduled = true;
        QTimer::singleShot(50, this, [this]() {
            m_initializationScheduled = false;
            ensureInitialized();
        });
    }
    event->accept();
}

void AnyLink::showMainWindow()
{
    if (m_quitRequested) {
        return;
    }

    ensureInitialized();
    show();
    showNormal();
    raise();
    activateWindow();

#if defined(Q_OS_MACOS)
    ForceActivation();
#endif
}

void AnyLink::ensureInitialized()
{
    if (m_initialized) {
        return;
    }

    m_initialized = true;
    afterShowOneTime();
}

void AnyLink::hideMainWindow()
{
    hide();
    if (trayIcon != nullptr && !trayIcon->isVisible()) {
        trayIcon->show();
    }
}

void AnyLink::quitApplication()
{
    if (m_quitRequested) {
        return;
    }

    m_quitRequested = true;
    timer.stop();
    hide();

    if (m_vpnConnected && rpc != nullptr && rpc->isConnected()) {
        disconnectVPN();
        // Give the agent a bounded window to process the disconnect request.
        QTimer::singleShot(1000, this, [this]() {
            if (m_quitRequested) {
                qApp->quit();
            }
        });
        return;
    }

    qApp->quit();
}

void AnyLink::center()
{
    QRect screenGeometry = screen()->geometry();
    QRect windowGeometry = frameGeometry();
    QPoint centerPoint = screenGeometry.center() - windowGeometry.center();
    if (screen()->devicePixelRatio() > 1.0) {
        centerPoint -= QPoint(0,120);
    }
    // 将窗口移动到居中位置
    move(centerPoint);
}

void AnyLink::loadStyleSheet(const QString &styleSheetFile)

{
    QFile file(styleSheetFile);
    file.open(QFile::ReadOnly);
    if (file.isOpen())
    {
        QString styleSheet = this->styleSheet();
        styleSheet += QLatin1String(file.readAll());//读取样式表文件
        setStyleSheet(styleSheet);//把文件内容传参
        file.close();
    }
}

void AnyLink::createTrayActions()
{
    actionConnect = new QAction(tr("Connect Gateway"), this);
    // not lambda must have this
    connect(actionConnect, &QAction::triggered, this, &AnyLink::connectVPN);

    actionDisconnect = new QAction(tr("Disconnect Gateway"), this);
    connect(actionDisconnect, &QAction::triggered, this, &AnyLink::disconnectVPN);

    actionConfig = new QAction(tr("Show Panel"), this);
    connect(actionConfig, &QAction::triggered, this, &AnyLink::showMainWindow);

    actionRetryAgent = new QAction(tr("Retry Local Agent Connection"), this);
    connect(actionRetryAgent, &QAction::triggered, this, &AnyLink::on_buttonAgentCheck_clicked);

    actionCopyDiagnostics = new QAction(tr("Copy Agent Diagnostics"), this);
    connect(actionCopyDiagnostics, &QAction::triggered,
            this, &AnyLink::on_buttonCopyDiagnostics_clicked);

    actionQuit = new QAction(tr("Quit"), this);
    connect(actionQuit, &QAction::triggered, this, &AnyLink::quitApplication);
}

void AnyLink::createTrayIcon()
{
    trayIconMenu = new QMenu(this);
    trayIconMenu->addAction(actionConnect);
    trayIconMenu->addAction(actionDisconnect);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(actionConfig);
    trayIconMenu->addAction(actionRetryAgent);
    trayIconMenu->addAction(actionCopyDiagnostics);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(actionQuit);

    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setContextMenu(trayIconMenu);
    trayIcon->setIcon(iconNotConnected);

#if defined(Q_OS_MACOS)
    // Note: On macOS, the Dock icon is used to provide the tray's functionality.
    MacDockIconHandler* dockIconHandler = MacDockIconHandler::instance();
    connect(dockIconHandler, &MacDockIconHandler::dockIconClicked, this, &AnyLink::showMainWindow);
    trayIconMenu->setAsDockMenu();
#endif

#if defined(Q_OS_WIN)
    connect(trayIcon,
            &QSystemTrayIcon::activated,
            this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    showMainWindow();
                }
            });
#endif
}

void AnyLink::initConfig()
{
    ui->checkBoxAutoLogin->setChecked(configManager->config["autoLogin"].toBool());
    ui->checkBoxMinimize->setChecked(configManager->config["minimize"].toBool());
    ui->checkBoxBlock->setChecked(configManager->config["block"].toBool());
    ui->checkBoxDebug->setChecked(configManager->config["debug"].toBool());
    ui->checkBoxLang->setChecked(configManager->config["local"].toBool());
    ui->checkBoxCiscoCompat->setChecked(configManager->config["cisco_compat"].toBool());
    ui->checkBoxDtls->setChecked(configManager->config["no_dtls"].toBool());
    ui->checkBoxNoDns->setChecked(configManager->config["no_dns"].toBool());

    connect(ui->checkBoxAutoLogin, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["autoLogin"] = checked;
        saveConfig();
    });
    connect(ui->checkBoxMinimize, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["minimize"] = checked;
        saveConfig();
    });
    connect(ui->checkBoxBlock, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["block"] = checked;
        configVPN();
        saveConfig();
    });
    connect(ui->checkBoxDebug, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["debug"] = checked;
        configVPN();
        saveConfig();
    });
    connect(ui->checkBoxLang, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["local"] = checked;
        saveConfig();
    });
    connect(ui->checkBoxCiscoCompat, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["cisco_compat"] = checked;
        configVPN();
        saveConfig();
    });
    connect(ui->checkBoxDtls, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["no_dtls"] = checked;
        configVPN();
        saveConfig();
    });
    connect(ui->checkBoxNoDns, &QCheckBox::toggled, this, [this](bool checked) {
        configManager->config["no_dns"] = checked;
        configVPN();
        saveConfig();
    });
}

void AnyLink::afterShowOneTime()
{
    createTrayActions();
    createTrayIcon();
    initConfig();
    profileManager->afterShowOneTime();
    detailDialog = new DetailDialog(this);

    // 每隔 60 秒获取 DTLS 状态，因为是 afterShowOneTime，不能关闭定时器
    connect(&timer, &QTimer::timeout, this, [this]() {
        if (!configManager->config["no_dtls"].toBool()) {
            rpc->callAsync("status", STATUS, [this](const QJsonValue &result) {
                const QJsonObject &status = result.toObject();
                if (!status.contains("code")) {
                    ui->labelChannelType->setText(status["DtlsConnected"].toBool() ? "DTLS" : "TLS");
                    ui->labelDtlsCipherSuite->setText(status["DTLSCipherSuite"].toString());
                }
            });
        }
    });

    connect(this, &AnyLink::vpnConnected, this, [this]() {
        getVPNStatus();
        m_vpnConnected = true;
        activeDisconnect = false;
        trayIcon->setIcon(iconConnected);
        ui->buttonConnect->setText(tr("Disconnect"));
        ui->comboBoxHost->setEnabled(false);
        ui->lineEditOTP->setEnabled(false);
        ui->buttonProfile->setEnabled(false);
        ui->tabSetting->setEnabled(false);

        actionConnect->setEnabled(false);
        actionDisconnect->setEnabled(true);
        if(ui->checkBoxMinimize->isChecked()) {
            hideMainWindow();
            trayIcon->setToolTip(tr("Connected to: ") + currentProfile.value("host").toString());
        }
        if (configManager->config["lastProfile"].toString() != ui->comboBoxHost->currentText()) {
            configManager->config["lastProfile"] = ui->comboBoxHost->currentText();
            saveConfig();
        }

        timer.start(60 * 1000);
    });

    connect(this, &AnyLink::vpnClosed, [this]() {
        m_vpnConnected = false;
        trayIcon->setIcon(iconNotConnected);
        trayIcon->setToolTip("");

        ui->buttonConnect->setText(tr("Connect"));
        ui->comboBoxHost->setEnabled(true);
        ui->lineEditOTP->setEnabled(true);
        ui->buttonProfile->setEnabled(true);
        ui->tabSetting->setEnabled(true);

        actionConnect->setEnabled(true);
        actionDisconnect->setEnabled(false);
        resetVPNStatus();

        timer.stop();
    });

    connect(qApp, &QApplication::aboutToQuit, this, [this]() {
        if(m_vpnConnected && !activeDisconnect) {
            disconnectVPN();
        }
    });

    rpc = new JsonRpcWebSocketClient(this);
    connect(rpc, &JsonRpcWebSocketClient::error, this, [this](const QString &error) {
        if (!error.isEmpty()) {
            ui->statusBar->setText(error);
        }
        if (m_vpnConnected) {
            emit vpnClosed();
        }
        if (isHidden() && !m_quitRequested) {
            showMainWindow();
        }
    });
    connect(rpc, &JsonRpcWebSocketClient::stateChanged, this,
            [this](JsonRpcWebSocketClient::ConnectionState state) {
                switch (state) {
                case JsonRpcWebSocketClient::ConnectionState::Connecting:
                    ui->statusBar->setText(tr("Connecting to the local vpnagent service..."));
                    break;
                case JsonRpcWebSocketClient::ConnectionState::Ready:
                    ui->statusBar->setText(tr("Local vpnagent service is ready."));
                    if (m_autoLoginRequested && !m_vpnConnected) {
                        m_autoLoginRequested = false;
                        QTimer::singleShot(0, this, [this]() { connectVPN(); });
                    }
                    break;
                case JsonRpcWebSocketClient::ConnectionState::Retrying:
                    ui->statusBar->setText(
                        tr("Waiting for the local vpnagent service (retry %1): %2")
                            .arg(rpc->retryCount())
                            .arg(rpc->lastError()));
                    break;
                case JsonRpcWebSocketClient::ConnectionState::Unavailable:
                    ui->statusBar->setText(
                        tr("Local vpnagent unavailable: %1. Check service status, port 6210, "
                           "and connection logs.")
                            .arg(rpc->lastError()));
                    break;
                case JsonRpcWebSocketClient::ConnectionState::ProtocolError:
                    ui->statusBar->setText(
                        tr("Local vpnagent protocol error: %1").arg(rpc->lastError()));
                    break;
                case JsonRpcWebSocketClient::ConnectionState::Disconnected:
                    if (!m_quitRequested) {
                        ui->statusBar->setText(tr("Local vpnagent service is disconnected."));
                    }
                    break;
                case JsonRpcWebSocketClient::ConnectionState::Unknown:
                    break;
                }
            });
    connect(rpc, &JsonRpcWebSocketClient::connectionError, this,
            [this](QAbstractSocket::SocketError socketError,
                   JsonRpcWebSocketClient::FailureCategory category,
                   const QString &error,
                   int retryCount,
                   bool willRetry) {
                Q_UNUSED(socketError)
                QString diagnosis;
                switch (category) {
                case JsonRpcWebSocketClient::FailureCategory::ServiceUnavailable:
                    diagnosis = tr("The service is not installed, not running, or launchd "
                                    "could not start it.");
                    break;
                case JsonRpcWebSocketClient::FailureCategory::PortConflictOrProtocol:
                    diagnosis = tr("Port 6210 may be occupied by another process or the "
                                    "local agent rejected the WebSocket protocol.");
                    break;
                case JsonRpcWebSocketClient::FailureCategory::TemporarilyUnavailable:
                    diagnosis = tr("The service may still be starting; the client will retry.");
                    break;
                case JsonRpcWebSocketClient::FailureCategory::ProtocolError:
                    diagnosis = tr("The service returned an invalid JSON-RPC response.");
                    break;
                case JsonRpcWebSocketClient::FailureCategory::Unknown:
                    diagnosis = tr("Check the agent architecture, signature, launchd state, "
                                    "and connection logs.");
                    break;
                case JsonRpcWebSocketClient::FailureCategory::None:
                    break;
                }
                if (willRetry) {
                    ui->statusBar->setText(
                        tr("Local vpnagent connection failed (retry %1): %2 %3")
                            .arg(retryCount)
                            .arg(diagnosis)
                            .arg(error));
                } else {
                    ui->statusBar->setText(
                        tr("Local vpnagent connection failed: %1 %2 Use Retry Local Agent "
                           "Connection or view the logs.")
                            .arg(diagnosis)
                            .arg(error));
                }
            });
    connect(rpc, &JsonRpcWebSocketClient::connected, this, [this]() {
        configVPN();
    });
    // may be exited normally by other clients, do not automatically reconnect
    rpc->registerCallback(DISCONNECT, [this](const QJsonValue & result) {
        ui->progressBar->stop();
        ui->statusBar->setText(result.toString());
        emit vpnClosed();
        if (m_quitRequested) {
            qApp->quit();
            return;
        }
        if(!activeDisconnect && isHidden()) {
            showMainWindow();
        }
    });
    // unusual exited
    rpc->registerCallback(ABORT, [this](const QJsonValue & result) {
        ui->statusBar->setText(result.toString());
        emit vpnClosed();
        if (!activeDisconnect) {
            // 快速重连，不需要再次进行用户认证
            QTimer::singleShot(1500, this, [this]() { connectVPN(true); });
        }
    });
    rpc->connectToServer(QUrl("ws://127.0.0.1:6210/rpc"));
}

void AnyLink::resetVPNStatus()
{
    ui->labelChannelType->clear();
    ui->labelTlsCipherSuite->clear();
    ui->labelDtlsCipherSuite->clear();
    ui->labelDTLSPort->clear();
    ui->labelServerAddress->clear();
    ui->labelLocalAddress->clear();
    ui->labelVPNAddress->clear();
    ui->labelMTU->clear();
    ui->labelDNS->clear();
    m_upstreamDns.clear();

    ui->buttonDetails->setEnabled(false);
    ui->buttonCopyDns->setEnabled(false);
    detailDialog->clear();

    ui->lineEditOTP->clear();
}

void AnyLink::saveConfig()
{
    configManager->saveConfig();
}

/**
 * called by JsonRpcWebSocketClient::connected and every time setting changed
 */
void AnyLink::configVPN()
{
    if(rpc->isConnected()) {
        QJsonObject args{{"log_level", ui->checkBoxDebug->isChecked() ? "Debug" : "Info"},
                         {"log_path", tempLocation},
                         {"skip_verify", !ui->checkBoxBlock->isChecked()},
                         {"cisco_compat", ui->checkBoxCiscoCompat->isChecked()},
                         {"no_dtls", ui->checkBoxDtls->isChecked()},
                         {"no_dns", ui->checkBoxNoDns->isChecked()},
                         {"agent_name", agentName},
                         {"agent_version", appVersion}};
        rpc->callAsync("config", CONFIG, args, [this](const QJsonValue & result) {
            ui->statusBar->setText(result.toString());
        });
    }
}

void AnyLink::connectVPN(bool reconnect)
{
    if(rpc->isConnected()) {
        // profile may be modified, and may not emit currentTextChanged signal
        // must not affected by QComboBox::currentTextChanged

        QString method = "connect";
        int id = CONNECT;
        if(reconnect) {
            method = "reconnect";
            id = RECONNECT;
        } else {
            const QString name = ui->comboBoxHost->currentText();
            if (name.isEmpty()) {
                return;
            }
            QJsonObject profile = profileManager->profiles[name].toObject();
            currentProfile = profile;
            const QString otp = ui->lineEditOTP->text();
            if(!otp.isEmpty()) {
                currentProfile["password"] = profile["password"].toString() + otp;
            }
        }
        ui->progressBar->start();
        trayIcon->setIcon(iconConnecting);

        rpc->callAsync(method, id, currentProfile, [this, reconnect](const QJsonValue &result) {
            ui->progressBar->stop();
            if(result.isObject()) {  // error object
                // dialog
                //                ui->statusBar->setText(result.toObject().value("message").toString());
                if (reconnect) {
                    // 当快速重连失败，再次尝试完全重新连接，用于服务端可能已经移除session的情况
                    QTimer::singleShot(3000, this, [this]() { connectVPN(); });
                } else {
                    if (isHidden()) {
                        showMainWindow();
                    }
                    error(result.toObject().value("message").toString(), this);
                }
            } else {
                ui->statusBar->setText(result.toString());
                emit vpnConnected();
            }
        });
    }
}

void AnyLink::disconnectVPN()
{
    if(rpc->isConnected()) {
        ui->progressBar->start();
        // because on_buttonConnect_clicked, must check m_vpnConnected outside
        rpc->callAsync("disconnect", DISCONNECT);
        activeDisconnect = true;
    }
}

void AnyLink::applyVpnStatus(const QJsonObject &status)
{
    if (status.contains("code")) {
        return;
    }

    ensureInitialized();

    ui->labelChannelType->setText(status["DtlsConnected"].toBool() ? "DTLS" : "TLS");
    ui->labelTlsCipherSuite->setText(status["TLSCipherSuite"].toString());
    ui->labelDtlsCipherSuite->setText(status["DTLSCipherSuite"].toString());
    ui->labelDTLSPort->setText(status["DTLSPort"].toString());
    ui->labelServerAddress->setText(status["ServerAddress"].toString());
    ui->labelLocalAddress->setText(status["LocalAddress"].toString());
    ui->labelVPNAddress->setText(status["VPNAddress"].toString());
    ui->labelMTU->setText(QString::number(status["MTU"].toInt()));

    QStringList dnsServers;
    for (const QString &entry : status["DNS"].toVariant().toStringList()) {
        const QString trimmed = entry.trimmed();
        if (!trimmed.isEmpty()) {
            dnsServers.append(trimmed);
        }
    }
    m_upstreamDns = dnsServers;

    QStringList splitDnsDomains;
    for (const QString &entry : status["SplitDNS"].toVariant().toStringList()) {
        const QString trimmed = entry.trimmed();
        if (!trimmed.isEmpty()) {
            splitDnsDomains.append(trimmed);
        }
    }

    const QString defaultDomain = status["DefaultDomain"].toString().trimmed();
    const bool noDnsRequested = configManager->config["no_dns"].toBool();
    const bool dnsAppliedKnown = status.contains("DNSApplied");
    const bool dnsApplied = dnsAppliedKnown ? status["DNSApplied"].toBool() : !dnsServers.isEmpty();
    const QJsonArray splitInclude = status["SplitInclude"].toArray();
    const QJsonArray splitExclude = status["SplitExclude"].toArray();
    const bool splitIncludeEmpty = splitInclude.isEmpty();

    if (dnsServers.isEmpty()) {
        ui->labelDNS->clear();
    } else if (noDnsRequested && !dnsAppliedKnown) {
        ui->labelDNS->setText(tr("%1 (Applied - update vpnagent)").arg(dnsServers.join(", ")));
    } else if (!dnsApplied) {
        ui->labelDNS->setText(tr("%1 (Not applied)").arg(dnsServers.join(", ")));
    } else {
        ui->labelDNS->setText(dnsServers.join(", "));
    }

    ui->buttonCopyDns->setEnabled(!dnsServers.isEmpty());

    if (!ui->buttonDetails->isEnabled()) {
        ui->buttonDetails->setEnabled(true);
    }
    detailDialog->setRoutes(splitExclude, splitInclude);

    DetailDialog::DnsInfo dnsInfo;
    dnsInfo.dnsServers = dnsServers;
    dnsInfo.defaultDomain = defaultDomain;
    dnsInfo.splitDnsDomains = splitDnsDomains;
    dnsInfo.noDnsRequested = noDnsRequested;
    dnsInfo.dnsAppliedKnown = dnsAppliedKnown;
    dnsInfo.dnsApplied = dnsApplied;
    dnsInfo.splitIncludeEmpty = splitIncludeEmpty;
    detailDialog->setDnsInfo(dnsInfo);

    if (noDnsRequested && !dnsAppliedKnown && !dnsServers.isEmpty()) {
        ui->statusBar->setText(
            tr("Local vpnagent does not support no_dns yet; system DNS may have been overwritten."));
    } else if (noDnsRequested && !dnsApplied && splitIncludeEmpty) {
        ui->statusBar->setText(
            tr("Connected without overwriting global DNS. Gateway pushed no split-include routes."));
    } else if (noDnsRequested && !dnsApplied && !dnsServers.isEmpty()) {
        ui->statusBar->setText(
            tr("Connected without overwriting global DNS. View or copy upstream DNS in Details."));
    }
}

void AnyLink::getVPNStatus()
{
    rpc->callAsync("status", STATUS, [this](const QJsonValue & result) {
        applyVpnStatus(result.toObject());
    });
}

void AnyLink::on_buttonConnect_clicked()
{
    if (rpc == nullptr || !rpc->isConnected()) {
        on_buttonAgentCheck_clicked();
        return;
    }

    if(m_vpnConnected) {
        disconnectVPN();
    } else {
        connectVPN();
    }
}

void AnyLink::on_buttonProfile_clicked()
{
    profileManager->exec();
}

void AnyLink::on_buttonViewLog_clicked()
{
    QString filePath = tempLocation + "/vpnagent.log";
    QFile loadFile(filePath);
    if(!loadFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error(tr("Couldn't open log file"), this);
        return;
    }
    TextBrowser textBrowser(tr("Log Viewer"),this);


    QString data = loadFile.readAll();
    textBrowser.setText(data);
    loadFile.close();

    // 创建文件系统监视器
    QFileSystemWatcher watcher;
    watcher.addPath(filePath);

    // 监视文件变化的信号槽连接
    QObject::connect(&watcher, &QFileSystemWatcher::fileChanged, [&]() {
        QFile updatedFile(filePath);
        if (updatedFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // 重新读取文件内容
            data = updatedFile.readAll();
            textBrowser.setText(data);
            updatedFile.close();
        }
    });

    textBrowser.exec();
}

void AnyLink::on_buttonAgentCheck_clicked()
{
    if (rpc == nullptr) {
        return;
    }

    ui->statusBar->setText(tr("Checking the local vpnagent service..."));
    rpc->retryNow();
}

void AnyLink::on_buttonCopyDiagnostics_clicked()
{
    if (rpc == nullptr) {
        return;
    }

    QApplication::clipboard()->setText(rpc->diagnosticReport());
    ui->statusBar->setText(tr("Agent diagnostics copied to the clipboard."));
}

void AnyLink::on_buttonDetails_clicked()
{
    detailDialog->exec();
}

void AnyLink::on_buttonCopyDns_clicked()
{
    if (m_upstreamDns.isEmpty()) {
        return;
    }

    const QString copied = detailDialog != nullptr
        ? detailDialog->copyAllDnsToClipboard()
        : m_upstreamDns.join(QStringLiteral(", "));
    if (detailDialog == nullptr) {
        QApplication::clipboard()->setText(copied);
    }
    ui->statusBar->setText(tr("Upstream DNS copied to the clipboard: %1").arg(copied));
}

void AnyLink::on_buttonSecurityTips_clicked()
{
    QString readme = "README.md";
    if (QLocale::system().name() == "zh_CN") {
        readme = "README_zh_CN.md";
    }
    QFile loadFile(":/resource/" + readme);
    if(!loadFile.open(QIODevice::ReadOnly)) {
        error(tr("Couldn't open README.md"), this);
        return;
    }
    QByteArray data = loadFile.readAll();
    TextBrowser textBrowser(tr("Security Tips"),this);
    textBrowser.setMarkdown(data);
    textBrowser.exec();
}
