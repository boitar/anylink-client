#include "detaildialog.h"
#include "ui_detaildialog.h"
#include "anylink.h"
#include "jsonrpcwebsocketclient.h"
#include <QApplication>
#include <QClipboard>
#include <QHostAddress>
#include <QJsonObject>

DetailDialog::DetailDialog(AnyLink *parent) :
    QDialog(parent),
    ui(new Ui::DetailDialog), anylink(parent)
{
    ui->setupUi(this);
    ui->tableExcluded->setColumnCount(2);
    ui->tableSecured->setColumnCount(2);
    ui->tableDns->setColumnCount(2);

    QStringList head = { tr("Destination"), tr("Prefix")};
    ui->tableExcluded->setHorizontalHeaderLabels(head);
    ui->tableSecured->setHorizontalHeaderLabels(head);
    ui->tableDns->setHorizontalHeaderLabels({ tr("Type"), tr("Value") });

    setFixedSize(geometry().width(), geometry().height());

    connect(ui->buttonCopyAllDns, &QPushButton::clicked, this, [this]() {
        copyAllDnsToClipboard();
    });
    connect(ui->buttonCopySelectedDns, &QPushButton::clicked, this, [this]() {
        copySelectedDnsToClipboard();
    });
    connect(ui->tableDns, &QTableWidget::itemSelectionChanged, this, [this]() {
        ui->buttonCopySelectedDns->setEnabled(ui->tableDns->currentRow() >= 0);
    });
    connect(ui->tableDns, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row >= 0) {
            ui->tableDns->selectRow(row);
            copySelectedDnsToClipboard();
        }
    });

    // 每隔 1 秒获取流量统计
    connect(&timer, &QTimer::timeout, this, [this]() {
        if(anylink != nullptr && anylink->rpc != nullptr && anylink->rpc->isConnected()) {
            anylink->rpc->callAsync("stat", AnyLink::STAT, [this](const QJsonValue & result) {
                const QJsonObject &stat = result.toObject();
                if(!stat.contains("code")) {
                    ui->labelBytesSent->setText(format(stat["bytesSent"].toDouble()));
                    ui->labelBytesReceived->setText(format(stat["bytesReceived"].toDouble()));
                }
            });
        }
    });
}

DetailDialog::~DetailDialog()
{
    delete ui;
}

void DetailDialog::setRoutes(const QJsonArray &excludes, const QJsonArray &includes)
{
    ui->tableExcluded->setRowCount(0);
    ui->tableSecured->setRowCount(0);

    for(int i = 0; i < excludes.size(); i++) {
        QPair<QHostAddress, int> cidr = QHostAddress::parseSubnet(excludes[i].toString());
        QTableWidgetItem *ip = new QTableWidgetItem(static_cast<QHostAddress>(cidr.first).toString());
        QTableWidgetItem *mask = new QTableWidgetItem(QString::number(cidr.second));

        ui->tableExcluded->insertRow(i);
        ui->tableExcluded->setItem(i, 0, ip);
        ui->tableExcluded->setItem(i, 1, mask);
    }

    for(int i = 0; i < includes.size(); i++) {
        QPair<QHostAddress, int> cidr = QHostAddress::parseSubnet(includes[i].toString());
        QTableWidgetItem *ip = new QTableWidgetItem(static_cast<QHostAddress>(cidr.first).toString());
        QTableWidgetItem *mask = new QTableWidgetItem(QString::number(cidr.second));

        ui->tableSecured->insertRow(i);
        ui->tableSecured->setItem(i, 0, ip);
        ui->tableSecured->setItem(i, 1, mask);
    }

    ui->tableExcluded->horizontalHeader()->resizeSections(QHeaderView::ResizeToContents);
    ui->tableSecured->horizontalHeader()->resizeSections(QHeaderView::ResizeToContents);

    ui->tableExcluded->setCurrentCell(-1, -1);
}

void DetailDialog::setDnsInfo(const DnsInfo &info)
{
    m_dnsServers = info.dnsServers;
    ui->tableDns->setRowCount(0);
    ui->labelDnsCopyFeedback->clear();

    int row = 0;
    for (const QString &server : info.dnsServers) {
        ui->tableDns->insertRow(row);
        ui->tableDns->setItem(row, 0, new QTableWidgetItem(tr("DNS Server")));
        ui->tableDns->setItem(row, 1, new QTableWidgetItem(server));
        row++;
    }

    if (!info.defaultDomain.isEmpty()) {
        ui->tableDns->insertRow(row);
        ui->tableDns->setItem(row, 0, new QTableWidgetItem(tr("Default Domain")));
        ui->tableDns->setItem(row, 1, new QTableWidgetItem(info.defaultDomain));
        row++;
    }

    for (const QString &domain : info.splitDnsDomains) {
        ui->tableDns->insertRow(row);
        ui->tableDns->setItem(row, 0, new QTableWidgetItem(tr("Split DNS Domain")));
        ui->tableDns->setItem(row, 1, new QTableWidgetItem(domain));
        row++;
    }

    ui->tableDns->horizontalHeader()->resizeSection(0, 150);
    if (ui->tableDns->rowCount() > 0) {
        ui->tableDns->selectRow(0);
    } else {
        ui->tableDns->setCurrentCell(-1, -1);
    }

    ui->buttonCopyAllDns->setEnabled(!m_dnsServers.isEmpty());
    ui->buttonCopySelectedDns->setEnabled(ui->tableDns->currentRow() >= 0);

    if (info.dnsServers.isEmpty()) {
        ui->labelDnsState->setText(tr("No upstream DNS servers were pushed by the gateway."));
    } else if (info.noDnsRequested && !info.dnsAppliedKnown) {
        ui->labelDnsState->setText(
            tr("Warning: local vpnagent did not report DNSApplied and may still have overwritten "
               "system DNS. Please update vpnagent."));
    } else if (!info.dnsApplied) {
        if (info.splitIncludeEmpty) {
            ui->labelDnsState->setText(
                tr("Not applied to system (global DNS override disabled). Gateway pushed no "
                   "split-include routes, so the system default route was not changed."));
        } else {
            ui->labelDnsState->setText(
                tr("Not applied to system (global DNS override disabled). Double-click a row or "
                   "use the buttons to copy for Surge / other DNS tools."));
        }
    } else {
        ui->labelDnsState->setText(tr("Applied to system DNS."));
    }
}

QString DetailDialog::copyAllDnsToClipboard()
{
    if (m_dnsServers.isEmpty()) {
        return QString();
    }

    const QString text = m_dnsServers.join(QStringLiteral(", "));
    QApplication::clipboard()->setText(text);
    updateCopyFeedback(text);
    return text;
}

QString DetailDialog::copySelectedDnsToClipboard()
{
    const int row = ui->tableDns->currentRow();
    if (row < 0) {
        return QString();
    }

    QTableWidgetItem *valueItem = ui->tableDns->item(row, 1);
    if (valueItem == nullptr || valueItem->text().isEmpty()) {
        return QString();
    }

    const QString text = valueItem->text();
    QApplication::clipboard()->setText(text);
    updateCopyFeedback(text);
    return text;
}

void DetailDialog::updateCopyFeedback(const QString &copiedText)
{
    ui->labelDnsCopyFeedback->setText(tr("Copied to clipboard: %1").arg(copiedText));
}

void DetailDialog::clear()
{
    m_dnsServers.clear();
    ui->tableExcluded->clearContents();
    ui->tableExcluded->setRowCount(0);
    ui->tableSecured->clearContents();
    ui->tableSecured->setRowCount(0);
    ui->tableDns->clearContents();
    ui->tableDns->setRowCount(0);

    ui->labelDnsState->clear();
    ui->labelDnsCopyFeedback->clear();
    ui->buttonCopyAllDns->setEnabled(false);
    ui->buttonCopySelectedDns->setEnabled(false);

    ui->labelBytesSent->clear();
    ui->labelBytesReceived->clear();
}

QString DetailDialog::format(double bytes)
{
    QString unit;
    // use 1000 not 1024 same as ip -h -s link show dev tun0
    if(bytes < 1000) {
        unit = "B";
    } else if(bytes < 1000000) {  // 1024*1024
        bytes /= 1000;
        unit = "KB";
    } else if(bytes < 1000000000) {  // 1024*1024*1024
        bytes /= 1000000;
        unit = "MB";
    } else {
        bytes /= 1000000000;
        unit = "GB";
    }
    return QString("%1 %2").arg(QString::number(bytes, 'f', 2)).arg(unit);
}


void DetailDialog::showEvent(QShowEvent *event)
{
    Q_UNUSED(event)
    timer.start(1000);
}


void DetailDialog::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event)
    timer.stop();
}
