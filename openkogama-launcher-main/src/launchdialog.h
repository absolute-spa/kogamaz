#ifndef LAUNCHDIALOG_H
#define LAUNCHDIALOG_H

#include <QDialog>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>
#include "paths.h"

class AssetInstaller;

namespace Ui {
class LaunchDialog;
}

class LaunchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LaunchDialog(const QString &version, QWidget *parent = nullptr);
    ~LaunchDialog();

    QProcess *server() const { return m_server; }
    static void stopServer(QProcess *server);

protected:
    void reject() override;

private:
    void checkServer();
    void startServer();
    void waitForServer(int attempts);
    void failed();
    void fail(const QString &message);
    void setStep(const QString &status, int progress);

    Ui::LaunchDialog *ui;
    QNetworkAccessManager *m_net;
    QPointer<QProcess> m_server;
    AssetInstaller *m_assets;
    int m_exitCode = 0;
    bool m_offline = false;
    bool m_cancelled = false;
    bool m_retried = false;
};

#endif // LAUNCHDIALOG_H
