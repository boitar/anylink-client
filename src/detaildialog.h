#ifndef DETAILDIALOG_H
#define DETAILDIALOG_H

#include <QDialog>
#include <QJsonArray>
#include <QStringList>
#include <QTimer>

namespace Ui {
class DetailDialog;
}

class AnyLink;
class DetailDialog : public QDialog
{
    Q_OBJECT

public:
    struct DnsInfo {
        QStringList dnsServers;
        QString defaultDomain;
        QStringList splitDnsDomains;
        bool noDnsRequested = false;
        bool dnsAppliedKnown = false;
        bool dnsApplied = true;
        bool splitIncludeEmpty = false;
    };

    explicit DetailDialog(AnyLink *parent);
    ~DetailDialog();

    void setRoutes(const QJsonArray &excludes, const QJsonArray &includes);
    void setDnsInfo(const DnsInfo &info);
    QString copyAllDnsToClipboard();
    QString copySelectedDnsToClipboard();
    void clear();
private:
    Ui::DetailDialog *ui;
    QTimer timer;
    AnyLink *anylink;
    QStringList m_dnsServers;

    QString format(double bytes);  // typedef quint64 qulonglong;
    void updateCopyFeedback(const QString &copiedText);
    // QWidget interface
protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
};

#endif // DETAILDIALOG_H
