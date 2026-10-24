#pragma once

#include <QList>
#include <QNetworkCookie>
#include <QObject>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace cj {

// QQ 频道 HTTP 接口（方案 A：不依赖浏览器）
//
// 上传（来自频道前端 trpc.qqntv2.richmedia.InnerProxy/Upload）：
//   POST https://pd.qq.com/qunng/guild/gotrpc/v1/trpc.qqntv2.richmedia.InnerProxy/Upload
//   multipart: file=<图片>, guild_id, channel_id
//   → data.compat_qq_msg 内含图片直链
// 其余 trpc 调用统一走代理：
//   POST https://pd.qq.com/qunng/guild/gotrpc/v1/trpc.group_pro.open_web_proxy.ConnectWebProxy/WebInvoke
//   body 里带 {cmd, serviceType, req}
class ChannelApi : public QObject
{
    Q_OBJECT

public:
    explicit ChannelApi(QObject *parent = nullptr);

    static QString guildId();
    static QString channelId();

    void setSessionCookies(const QList<QNetworkCookie> &cookies);

    // 上传一张载体，返回图片直链（失败时 error 非空）
    // index = 本次发放里的第几张；progress 每发一段字节回调一次，用于汇总百分比
    void uploadImage(const QString &path, int index,
                     std::function<void(bool, QString url, QString error)> callback,
                     std::function<void(qint64 sent, qint64 total)> progress = {});

    // 发帖（body 结构按频道前端构造）
    void publishFeed(const QString &content,
                     const QList<QString> &compatMsgs,
                     std::function<void(bool, QString error)> callback);

private:
    QNetworkAccessManager *m_manager = nullptr;
};

} // namespace cj
