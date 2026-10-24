#pragma once

#include "core/creator.h"
#include "core/restorer.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace cj {

struct ClientUpdateInfo
{
    bool available = false;
    QString currentVersion;
    QString version;
    QString downloadUrl;
    QString sha256;
    qint64 size = 0;
    QString notes;
    bool force = false;
    qint64 publishedAt = 0;
};

// 与 feijipan-api 参考实现逐字节一致的小飞机请求参数加密。
QString feijiiEncryptParameter(const QString &plain, QString *error = nullptr);

// 后端口令服务（与安卓同一后端、同一协议）
//   action=tm-create  → 登记资源并获得 5 位字母口令
//   action=tm-get     → 按口令读取资源
class Backend : public QObject
{
    Q_OBJECT

public:
    using JsonCallback = std::function<void(bool ok, const QJsonObject &data, const QString &error)>;
    using FileCallback = std::function<void(bool ok, const QString &error)>;
    using FileProgressCallback = std::function<void(qint64 received, qint64 total)>;
    using UrlCallback = std::function<void(bool ok,
                                           const QString &directUrl,
                                           const QString &fileName,
                                           qint64 fileSize,
                                           const QString &error)>;
    using UpdateCallback =
        std::function<void(bool ok, const ClientUpdateInfo &info, const QString &error)>;

    explicit Backend(QObject *parent = nullptr);

    static QString defaultApiBase();

    void checkClientUpdate(const QString &currentVersion, UpdateCallback callback);

    // 登记：urls 必须与 carriers 一一对应，返回 5 位字母口令
    void registerResource(const CreatedResource &resource,
                          const QStringList &urls,
                          std::function<void(bool ok, const QString &code, const QString &error)> callback);

    // 读取记录
    void fetchRecord(const QString &code, std::function<void(bool ok, const ResourceRecord &, const QString &error)> callback);

    // 下载分片到本地路径（校验远端 SHA-256 由调用方完成）
    void downloadFile(const QString &url,
                      const QString &path,
                      FileCallback callback,
                      FileProgressCallback progress = {});

    // 把小飞机永久分享链接解析为当前有效的 CDN 下载地址。
    void resolveFeijiiShare(const QString &shareUrl, UrlCallback callback);

signals:
    void progressChanged(qreal value, const QString &stage);

private:
    void post(const QJsonObject &body, JsonCallback callback);

    QNetworkAccessManager *m_manager = nullptr;
};

} // namespace cj
