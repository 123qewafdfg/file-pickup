#include "qq/channelapi.h"

#include "core/logger.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace cj {

namespace {

constexpr const char *kUploadUrl =
    "https://pd.qq.com/qunng/guild/gotrpc/v1/trpc.qqntv2.richmedia.InnerProxy/Upload";
constexpr const char *kProxyUrl =
    "https://pd.qq.com/qunng/guild/gotrpc/v1/trpc.group_pro.open_web_proxy.ConnectWebProxy/WebInvoke";

constexpr const char *kGuildId = "16670831792565266";
constexpr const char *kChannelId = "744499928";

QString userAgent()
{
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                          "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
}

// compat_qq_msg 是 base64 的 protobuf，里面的图片地址是明文可打印字符串，
// 形如 qmeetpic/<guild>/<channel>-<...>/<seq>?term=255，需要拼 CDN 前缀。
QString scanPrintableRuns(const QByteArray &bin, const QString &needle)
{
    QString best;
    QByteArray run;
    const QByteArray key = needle.toUtf8();
    auto flush = [&]() {
        if (run.size() >= 8) {
            const int at = run.indexOf(key);
            if (at >= 0)
                best = QString::fromUtf8(run.mid(at));
        }
        run.clear();
    };
    for (char c : bin) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 0x20 && u < 0x7f) || u == '&' || u == '?' || u == '%' || u == '=' || u == '/')
            run.append(c);
        else
            flush();
    }
    flush();
    return best;
}

// 调试用：把 protobuf 里 >=5 字节的可打印串全部列出
QString dumpRuns(const QByteArray &bin)
{
    QStringList out;
    QByteArray run;
    auto flush = [&]() {
        if (run.size() >= 5)
            out << QString::fromUtf8(run);
        run.clear();
    };
    for (char c : bin) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7f)
            run.append(c);
        else
            flush();
    }
    flush();
    return out.join(QStringLiteral(" | "));
}

QString extractImageUrl(const QJsonObject &data)
{
    const QString compat = data.value(QStringLiteral("compat_qq_msg")).toString();
    if (!compat.isEmpty()) {
        const QByteArray bin = QByteArray::fromBase64(compat.toLatin1());
        logger::event(QStringLiteral("api.upload.runs"), dumpRuns(bin));
        // 同一张图会给多个档位（/0=原图，/198、/720 是缩略），只取原图那一档
        const QStringList runs = dumpRuns(bin).split(QStringLiteral(" | "));
        for (const QString &run : runs) {
            if (!run.contains(QStringLiteral("qmeetpic/")) || !run.contains(QStringLiteral("/0?term=")))
                continue;
            const int at = run.indexOf(QStringLiteral("qmeetpic/"));
            return QStringLiteral("https://gchat.qpic.cn/") + run.mid(at);
        }
        const QJsonDocument doc = QJsonDocument::fromJson(compat.toUtf8());
        if (doc.isObject()) {
            const QString text = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
            // 依次尝试常见字段
            for (const QString &key : {QStringLiteral("\"pic_url\""), QStringLiteral("\"url\""),
                                       QStringLiteral("\"originUrl\""), QStringLiteral("\"picUrl\"")}) {
                const int at = text.indexOf(key);
                if (at < 0)
                    continue;
                const int start = text.indexOf(QLatin1Char('"'), at + key.size() + 1);
                if (start < 0)
                    continue;
                const int end = text.indexOf(QLatin1Char('"'), start + 1);
                if (end < 0)
                    continue;
                const QString value = text.mid(start + 1, end - start - 1);
                if (value.startsWith(QStringLiteral("http")))
                    return value;
            }
        }
    }
    // 退路：整个响应里找图床地址
    const QString whole = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
    const int at = whole.indexOf(QStringLiteral("photo.store.qq.com"));
    if (at > 0) {
        int begin = at;
        while (begin > 0 && whole.at(begin - 1) != QLatin1Char('"'))
            --begin;
        const int end = whole.indexOf(QLatin1Char('"'), at);
        if (end > at)
            return whole.mid(begin, end - begin);
    }
    return {};
}

} // namespace

ChannelApi::ChannelApi(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
{
    m_manager->setCookieJar(new QNetworkCookieJar(this));
}

QString ChannelApi::guildId()
{
    return QString::fromLatin1(kGuildId);
}

QString ChannelApi::channelId()
{
    return QString::fromLatin1(kChannelId);
}

void ChannelApi::setSessionCookies(const QList<QNetworkCookie> &cookies)
{
    auto *jar = m_manager->cookieJar();
    if (!jar)
        return;
    for (const QNetworkCookie &cookie : cookies)
        jar->insertCookie(cookie);
    logger::event(QStringLiteral("api.cookies"), QStringLiteral("count=%1").arg(cookies.size()));
}

void ChannelApi::uploadImage(const QString &path, int index,
                             std::function<void(bool, QString, QString)> callback,
                             std::function<void(qint64, qint64)> progress)
{
    const QFileInfo info(path);
    if (!info.exists() || info.size() <= 0) {
        if (callback)
            callback(false, {}, QStringLiteral("载体文件不存在"));
        return;
    }

    // 请求体先落成临时文件，再把文件当上传流交给 QNAM：
    //  - 29 MiB × 18 张并发时不在内存里堆 body
    //  - 有明确的 Content-Length，进度也能按字节回流
    const QString boundary = QStringLiteral("----cangjian%1")
                                 .arg(QDateTime::currentMSecsSinceEpoch());
    const QString bodyPath = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                                 .filePath(QStringLiteral("upload-%1.bin")
                                               .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    QFile bodyFile(bodyPath);
    if (!bodyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (callback)
            callback(false, {}, QStringLiteral("无法创建上传临时文件"));
        return;
    }

    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        bodyFile.close();
        QFile::remove(bodyPath);
        if (callback)
            callback(false, {}, QStringLiteral("无法读取载体文件"));
        return;
    }

    auto writeField = [&bodyFile, &boundary](const QString &name, const QString &value) {
        bodyFile.write(QStringLiteral("--%1\r\n").arg(boundary).toUtf8());
        bodyFile.write(QStringLiteral("Content-Disposition: form-data; name=\"%1\"\r\n\r\n")
                           .arg(name)
                           .toUtf8());
        bodyFile.write(value.toUtf8());
        bodyFile.write("\r\n");
    };
    writeField(QStringLiteral("guild_id"), guildId());
    writeField(QStringLiteral("channel_id"), channelId());

    bodyFile.write(QStringLiteral("--%1\r\n").arg(boundary).toUtf8());
    bodyFile.write(QStringLiteral("Content-Disposition: form-data; name=\"file\"; filename=\"%1\"\r\n")
                       .arg(info.fileName())
                       .toUtf8());
    bodyFile.write("Content-Type: image/png\r\n\r\n");

    while (!source.atEnd()) {
        const QByteArray chunk = source.read(4 * 1024 * 1024);
        if (chunk.isEmpty())
            break;
        if (bodyFile.write(chunk) != chunk.size()) {
            bodyFile.close();
            source.close();
            QFile::remove(bodyPath);
            if (callback)
                callback(false, {}, QStringLiteral("写入上传临时文件失败（磁盘空间不足？）"));
            return;
        }
    }
    source.close();
    bodyFile.write("\r\n");
    bodyFile.write(QStringLiteral("--%1--\r\n").arg(boundary).toUtf8());
    bodyFile.close();

    QNetworkRequest request{QUrl(QString::fromLatin1(kUploadUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("multipart/form-data; boundary=%1").arg(boundary));
    request.setHeader(QNetworkRequest::ContentLengthHeader, QFileInfo(bodyPath).size());
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader("Origin", "https://pd.qq.com");
    request.setRawHeader("Referer", "https://pd.qq.com/g/pd55235600");
    request.setTransferTimeout(300000);

    logger::event(QStringLiteral("api.upload.start"),
                  QStringLiteral("idx=%1 file=%2 bytes=%3 body=%4")
                      .arg(index).arg(info.fileName()).arg(info.size()).arg(QFileInfo(bodyPath).size()));

    auto *upload = new QFile(bodyPath);
    if (!upload->open(QIODevice::ReadOnly)) {
        delete upload;
        QFile::remove(bodyPath);
        if (callback)
            callback(false, {}, QStringLiteral("无法打开上传临时文件"));
        return;
    }

    logger::event(QStringLiteral("api.upload.posting"), QStringLiteral("idx=%1").arg(index));
    QNetworkReply *reply = m_manager->post(request, upload);
    logger::event(QStringLiteral("api.upload.posted"), QStringLiteral("idx=%1").arg(index));

    if (progress) {
        connect(reply, &QNetworkReply::uploadProgress, this,
                [progress](qint64 sent, qint64 total) {
                    if (sent == total || sent == 0)
                        logger::event(QStringLiteral("api.upload.tick"),
                                      QStringLiteral("sent=%1 total=%2").arg(sent).arg(total));
                    progress(sent, total);
                });
    }
    upload->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [reply, callback, bodyPath]() {
        reply->deleteLater();
        QFile::remove(bodyPath);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        logger::event(QStringLiteral("api.upload.reply"),
                      QStringLiteral("http=%1 bytes=%2").arg(status).arg(raw.size()));

        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            if (callback)
                callback(false, {}, QStringLiteral("上传响应无法解析（HTTP %1）").arg(status));
            return;
        }
        const QJsonObject root = doc.object();
        const int retcode = root.value(QStringLiteral("retcode")).toInt(0);
        if (retcode != 0 || status < 200 || status > 299) {
            if (callback)
                callback(false, {},
                         QStringLiteral("上传失败 retcode=%1 %2")
                             .arg(retcode)
                             .arg(root.value(QStringLiteral("message")).toString()));
            return;
        }
        const QString url = extractImageUrl(root.value(QStringLiteral("data")).toObject());
        if (url.isEmpty()) {
            logger::event(QStringLiteral("api.upload.no-url"), QString::fromUtf8(raw));
            if (callback)
                callback(false, {}, QStringLiteral("上传成功但没解析出图片地址"));
            return;
        }
        logger::event(QStringLiteral("api.upload.ok"), url.left(140));
        if (callback)
            callback(true, url, {});
    });
}

void ChannelApi::publishFeed(const QString &content,
                             const QList<QString> &compatMsgs,
                             std::function<void(bool, QString)> callback)
{
    // 频道前端：PublishFeed 走代理 WebInvoke，cmd=0x92a4
    QJsonObject payload;
    payload.insert(QStringLiteral("cmd"), QStringLiteral("0x92a4"));
    payload.insert(QStringLiteral("serviceType"), QStringLiteral("1"));

    QJsonObject req;
    req.insert(QStringLiteral("guild_id"), guildId());
    req.insert(QStringLiteral("channel_id"), channelId());
    req.insert(QStringLiteral("content"), content);
    QJsonArray elements;
    for (const QString &msg : compatMsgs) {
        const QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
        if (doc.isObject())
            elements.append(doc.object());
    }
    req.insert(QStringLiteral("elements"), elements);
    payload.insert(QStringLiteral("req"), req);

    QNetworkRequest request{QUrl(QString::fromLatin1(kProxyUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader("x-oidb", R"({"uint32_service_type":1})");
    request.setRawHeader("Origin", "https://pd.qq.com");
    request.setRawHeader("Referer", "https://pd.qq.com/g/pd55235600");
    request.setTransferTimeout(60000);

    QNetworkReply *reply = m_manager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        logger::event(QStringLiteral("api.publish.reply"),
                      QStringLiteral("http=%1 body=%2").arg(status).arg(QString::fromUtf8(raw.left(300))));
        if (status < 200 || status > 299) {
            if (callback)
                callback(false, QStringLiteral("发帖失败（HTTP %1）").arg(status));
            return;
        }
        if (callback)
            callback(true, {});
    });
}

} // namespace cj
