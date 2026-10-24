#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkCookie>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace cj {

// QQ 二维码登录（官方 ptlogin2 流程）
//   1. GET xlogin       → 拿 pt_login_sig
//   2. GET ptqrshow     → 二维码 PNG + qrsig cookie
//   3. 轮询 ptqrlogin   → 66 未扫描 / 67 已扫描待确认 / 0 成功
//   4. GET check_sig    → 换取会话 cookie
// 安全：cookie 值只留在内存，不打印、不落盘、不上报。
class QqQrLogin : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QString qrImagePath READ qrImagePath NOTIFY qrImagePathChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit QqQrLogin(QObject *parent = nullptr);

    QString state() const { return m_state; }
    QString message() const { return m_message; }
    QString qrImagePath() const { return m_qrImagePath; }
    bool loggedIn() const { return m_loggedIn; }
    bool busy() const { return m_busy; }

    // 恢复上次保存的会话（DPAPI 加密的本机文件）
    Q_INVOKABLE bool restoreSession();
    Q_INVOKABLE void forgetSession();
    Q_INVOKABLE void start();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void abort();
    // 登录成功后把会话（cookie 名 + 是否持有）摘要给上层判断，不暴露值
    Q_INVOKABLE QString cookieNames() const;
    // 供 WebEngine 复用登录态（不落盘、不打日志）
    QList<QNetworkCookie> sessionCookies() const;

signals:
    void stateChanged();
    void messageChanged();
    void qrImagePathChanged();
    void loggedInChanged();
    void busyChanged();
    void loginSucceeded();

private:
    void setState(const QString &state, const QString &message);
    void setBusy(bool busy);
    void fetchLoginSig();
    void fetchQrImage();
    void startPolling();
    void pollOnce();
    void finishLogin(const QString &checkSigUrl);
    void fail(const QString &message);
    QByteArray cookieValue(const QString &name) const;
    void saveSession() const;

    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_pollReply = nullptr;
    QByteArray m_loginSig;
    QByteArray m_qrsig;
    QByteArray m_ptqrtoken;
    QString m_state = QStringLiteral("idle");
    QString m_message;
    QString m_qrImagePath;
    bool m_loggedIn = false;
    bool m_busy = false;
    int m_pollCount = 0;
};

} // namespace cj
