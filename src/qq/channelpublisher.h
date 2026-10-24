#pragma once

#include <QList>
#include <QNetworkCookie>
#include <QObject>
#include <QStringList>
#include <QUrl>

#include <functional>

class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;
class QWidget;

namespace cj {

// QQ 频道上传/发帖。
//
// 频道没有开放上传接口，上传/发帖由页面自身 JS 完成；
// Chromium 只接受"用户手势"触发的文件选择框（脚本给 input.files 赋值会被忽略），
// 因此这里用隐藏的 QWebEngineView 打开频道页，用合成鼠标点击上传按钮，
// 再由 chooseFiles 回调把载体 PNG 交给页面。
//
// 两种模式：
//   publish = true  → 上传 + 点发布，频道里产生真实帖子（长期直链）
//   publish = false → 只上传不点发布（临时直链）
class ChannelPublisher : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)

public:
    explicit ChannelPublisher(QObject *parent = nullptr);
    ~ChannelPublisher() override;

    QString state() const { return m_state; }
    QString message() const { return m_message; }

    void setSessionCookies(const QList<QNetworkCookie> &cookies);

    Q_INVOKABLE void start(const QStringList &files, bool publish,
                           const QString &channelUrl = QStringLiteral("https://pd.qq.com/g/pd55235600"));
    Q_INVOKABLE void abort();
    Q_INVOKABLE QString dumpPage();
    Q_INVOKABLE void showWindow(bool visible);

signals:
    void stateChanged();
    void messageChanged();
    void progress(const QString &stage);
    void finished(const QStringList &urls);
    void failed(const QString &message);

private:
    void setState(const QString &state, const QString &message);
    void ensureWindow();
    void stepOpenChannel();
    void stepOpenEditor();
    void stepClickUpload();
    void pollPreview();
    void stepPublish();
    void verifyPublish(int attempt = 0);
    void stepCollectLinks();
    void runJs(const QString &script, std::function<void(const QVariant &)> callback = nullptr);
    bool clickAtCssPoint(qreal x, qreal y);
    QStringList previewUrls(const QVariant &value) const;

    QWebEngineProfile *m_profile = nullptr;
    QWidget *m_window = nullptr;
    QWebEngineView *m_view = nullptr;
    QWebEnginePage *m_page = nullptr;

    QStringList m_pendingFiles;
    QStringList m_files;
    QString m_channelUrl;
    QString m_state = QStringLiteral("idle");
    QString m_message;
    bool m_publish = true;
    int m_pollTicks = 0;
    int m_clickedPublish = 0;
};

} // namespace cj
