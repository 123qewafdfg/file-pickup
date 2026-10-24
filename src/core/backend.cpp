#include "core/backend.h"

#include "core/crypto.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cj {

namespace {

// aria2c 的查找顺序：CJ_ARIA2 → exe 同目录 → 编译期默认路径（开发机）
QString aria2Executable()
{
    const QString fromEnv = qEnvironmentVariable("CJ_ARIA2");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv))
        return fromEnv;
    const QString beside = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("aria2c.exe"));
    if (QFileInfo::exists(beside))
        return beside;
#ifdef CJ_ARIA2_DEFAULT
    const QString bundled = QStringLiteral(CJ_ARIA2_DEFAULT);
    if (QFileInfo::exists(bundled))
        return bundled;
#endif
    return {};
}

QString jsonScalarText(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(static_cast<qint64>(value.toDouble()));
    return {};
}

QByteArray feijiiCompatKey(QString *error)
{
    QByteArray bootstrap = QByteArrayLiteral("AES/ECB/PKCS5Padding").left(16);
    const QByteArray encrypted = QByteArray::fromBase64(
        QByteArrayLiteral("YbQHZqK/PdQql2+7ATcPQHREAxt0Hn0Ob9v317QirZM="));
    QByteArray keyText = aesEcbDecryptPkcs7(bootstrap, encrypted, error);
    if (keyText.isEmpty())
        return {};
    if (keyText.size() > 16)
        keyText.truncate(16);
    while (keyText.size() < 16)
        keyText.append('L');
    return keyText;
}

QString feijiiUuid()
{
    static const QByteArray alphabet =
        QByteArrayLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
    const QByteArray bytes = randomBytes(21);
    QByteArray out;
    out.reserve(bytes.size());
    for (unsigned char byte : bytes)
        out.append(alphabet.at(byte % alphabet.size()));
    return QString::fromLatin1(out);
}

bool isFeijiiShareUrl(const QUrl &url)
{
    const QString host = url.host().toLower();
    return (host == QStringLiteral("feejii.com") || host.endsWith(QStringLiteral(".feejii.com"))
            || host == QStringLiteral("feijix.com") || host.endsWith(QStringLiteral(".feijix.com"))
            || host == QStringLiteral("feijipan.com") || host.endsWith(QStringLiteral(".feijipan.com")))
           && url.path().startsWith(QStringLiteral("/s/"));
}

void configureFeijiiRequest(QNetworkRequest *request)
{
    request->setRawHeader("Accept", "application/json, text/plain, */*");
    request->setRawHeader("Origin", "https://www.feijix.com");
    request->setRawHeader("Referer", "https://www.feijix.com/");
    request->setRawHeader("User-Agent",
                          "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                          "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
    request->setTransferTimeout(30000);
}

} // namespace

QString feijiiEncryptParameter(const QString &plain, QString *error)
{
    const QByteArray key = feijiiCompatKey(error);
    if (key.isEmpty())
        return {};
    return QString::fromLatin1(aesEcbEncryptPkcs7(key, plain.toUtf8(), error).toHex());
}

Backend::Backend(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
{
}

QString Backend::defaultApiBase()
{
    return QStringLiteral("https://env-00jy6xru2hon.dev-hz.cloudbasefunction.cn/share");
}

void Backend::checkClientUpdate(const QString &currentVersion, UpdateCallback callback)
{
    QJsonObject body;
    body.insert(QStringLiteral("action"), QStringLiteral("client-update-check"));
    body.insert(QStringLiteral("appId"), QStringLiteral("file-pickup"));
    body.insert(QStringLiteral("platform"), QStringLiteral("windows-x64"));
    body.insert(QStringLiteral("channel"), QStringLiteral("stable"));
    body.insert(QStringLiteral("currentVersion"), currentVersion);
    post(body, [callback = std::move(callback)](bool ok,
                                                const QJsonObject &data,
                                                const QString &error) {
        ClientUpdateInfo info;
        if (!ok) {
            if (callback)
                callback(false, info, error);
            return;
        }
        info.available = data.value(QStringLiteral("available")).toBool();
        info.currentVersion = data.value(QStringLiteral("currentVersion")).toString();
        if (info.available) {
            info.version = data.value(QStringLiteral("version")).toString();
            info.downloadUrl = data.value(QStringLiteral("downloadUrl")).toString();
            info.sha256 = data.value(QStringLiteral("sha256")).toString();
            info.size = static_cast<qint64>(data.value(QStringLiteral("size")).toDouble());
            info.notes = data.value(QStringLiteral("notes")).toString();
            info.force = data.value(QStringLiteral("force")).toBool();
            info.publishedAt =
                static_cast<qint64>(data.value(QStringLiteral("publishedAt")).toDouble());
            const QUrl download(info.downloadUrl);
            if (info.version.isEmpty() || !download.isValid()
                || download.scheme() != QStringLiteral("https")) {
                if (callback)
                    callback(false, {}, QStringLiteral("后端返回的更新信息无效"));
                return;
            }
        }
        if (callback)
            callback(true, info, {});
    });
}

void Backend::post(const QJsonObject &body, JsonCallback callback)
{
    QNetworkRequest request{QUrl(defaultApiBase())};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json; charset=utf-8"));
    request.setTransferTimeout(60000);

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_manager->post(request, payload);

    connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();

        if (reply->error() != QNetworkReply::NoError && status == 0) {
            if (callback)
                callback(false, {}, QStringLiteral("网络请求失败：%1").arg(reply->errorString()));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (callback)
                callback(false, {}, QStringLiteral("后端响应无法解析（HTTP %1）").arg(status));
            return;
        }
        const QJsonObject root = doc.object();
        if (status < 200 || status > 299 || !root.value(QStringLiteral("ok")).toBool()) {
            const QString message = root.value(QStringLiteral("message")).toString();
            if (callback)
                callback(false,
                         {},
                         message.isEmpty()
                             ? QStringLiteral("后端请求失败（HTTP %1）").arg(status)
                             : message);
            return;
        }
        const QJsonObject data = root.value(QStringLiteral("data")).toObject();
        if (callback)
            callback(true, data, {});
    });
}

void Backend::registerResource(
    const CreatedResource &resource,
    const QStringList &urls,
    std::function<void(bool, const QString &, const QString &)> callback)
{
    if (urls.size() != resource.parts.size()) {
        if (callback)
            callback(false, {}, QStringLiteral("远端图片数量与分片数量不一致"));
        return;
    }

    QJsonArray parts;
    for (int i = 0; i < resource.parts.size(); ++i) {
        const CarrierPart &part = resource.parts.at(i);
        QJsonObject entry;
        entry.insert(QStringLiteral("partIndex"), i);
        entry.insert(QStringLiteral("downloadUrl"), urls.at(i));
        entry.insert(QStringLiteral("remoteSha256"), part.remoteSha256Hex);
        entry.insert(QStringLiteral("cipherSha256"), part.cipherSha256Hex);
        entry.insert(QStringLiteral("cipherSize"), static_cast<double>(part.cipherSize));
        parts.append(entry);
    }

    QJsonObject body;
    body.insert(QStringLiteral("action"), QStringLiteral("tm-create"));
    body.insert(QStringLiteral("idFormat"), QStringLiteral("letters5"));
    body.insert(QStringLiteral("protocolVersion"), 1);
    body.insert(QStringLiteral("fileKey"), base64Url(resource.fileKey));
    body.insert(QStringLiteral("encryptedMetadata"), base64Url(resource.encryptedMetadata));
    body.insert(QStringLiteral("originalSha256"), resource.originalSha256Hex);
    body.insert(QStringLiteral("parts"), parts);

    post(body, [callback = std::move(callback)](bool ok, const QJsonObject &data, const QString &error) {
        if (!ok) {
            if (callback)
                callback(false, {}, error);
            return;
        }
        QString code = data.value(QStringLiteral("id")).toString().trimmed().toUpper();
        static const QRegularExpression pattern(QStringLiteral("^[A-Z]{5}$"));
        if (!pattern.match(code).hasMatch()) {
            if (callback)
                callback(false, {}, QStringLiteral("后端返回的口令格式不正确"));
            return;
        }
        if (callback)
            callback(true, code, {});
    });
}

void Backend::fetchRecord(
    const QString &code,
    std::function<void(bool, const ResourceRecord &, const QString &)> callback)
{
    QJsonObject body;
    body.insert(QStringLiteral("action"), QStringLiteral("tm-get"));
    body.insert(QStringLiteral("id"), code.trimmed().toUpper());

    post(body, [callback = std::move(callback)](bool ok, const QJsonObject &data, const QString &error) {
        if (!ok) {
            if (callback)
                callback(false, {}, error);
            return;
        }

        ResourceRecord record;
        record.protocolVersion = data.value(QStringLiteral("protocolVersion")).toInt(1);
        record.fileKey = fromBase64Url(data.value(QStringLiteral("fileKey")).toString());
        record.encryptedMetadata =
            fromBase64Url(data.value(QStringLiteral("encryptedMetadata")).toString());
        record.originalSha256Hex = data.value(QStringLiteral("originalSha256")).toString();

        const QJsonArray parts = data.value(QStringLiteral("parts")).toArray();
        for (const QJsonValue &value : parts) {
            const QJsonObject item = value.toObject();
            ResourcePart part;
            part.partIndex = item.value(QStringLiteral("partIndex")).toInt();
            part.downloadUrl = item.value(QStringLiteral("downloadUrl")).toString();
            part.remoteSha256Hex = item.value(QStringLiteral("remoteSha256")).toString();
            part.cipherSha256Hex = item.value(QStringLiteral("cipherSha256")).toString();
            part.cipherSize = static_cast<qint64>(item.value(QStringLiteral("cipherSize")).toDouble());
            record.parts.append(part);
        }

        if (record.fileKey.size() != 32) {
            if (callback)
                callback(false, {}, QStringLiteral("口令记录中的密钥无效"));
            return;
        }
        if (callback)
            callback(true, record, {});
    });
}

void Backend::downloadFile(const QString &url,
                           const QString &path,
                           FileCallback callback,
                           FileProgressCallback progress)
{
    if (isFeijiiShareUrl(QUrl(url))) {
        resolveFeijiiShare(
            url,
            [this, path, callback = std::move(callback), progress](bool ok,
                                                                   const QString &directUrl,
                                                                   const QString &,
                                                                   qint64,
                                                                   const QString &error) mutable {
                if (!ok) {
                    if (callback)
                        callback(false, error);
                    return;
                }
                downloadFile(directUrl, path, std::move(callback), progress);
            });
        return;
    }

    // 只有 B 站 CDN 能用 aria2：QQ 频道的 CDN 收到 Range 头会返回另一份内容
    // （实测无 Range 1,680,630 字节 / 带 Range 3,310,132 字节），分片下载必然校验失败
    const QString host = QUrl(url).host();
    const bool biliHost = host.endsWith(QStringLiteral("hdslb.com"))
                          || host.endsWith(QStringLiteral("biliimg.com"));
    const QString aria2 = biliHost ? aria2Executable() : QString();
    if (!aria2.isEmpty()) {
        const QFileInfo target(path);
        QDir().mkpath(target.absolutePath());
        QFile::remove(path);   // aria2 复用旧文件会算错长度

        auto *proc = new QProcess(this);
        proc->setProcessChannelMode(QProcess::MergedChannels);
        proc->setProperty("lastProgress", -1);
        connect(proc, &QProcess::readyReadStandardOutput, this, [proc, progress]() {
            const QString text = QString::fromLocal8Bit(proc->readAllStandardOutput());
            static const QRegularExpression percentPattern(QStringLiteral(R"(\((\d{1,3})%\))"));
            QRegularExpressionMatchIterator matches = percentPattern.globalMatch(text);
            int percent = -1;
            while (matches.hasNext())
                percent = matches.next().captured(1).toInt();
            if (percent >= 0)
                percent = qBound(0, percent, 100);
            if (percent >= 0 && percent != proc->property("lastProgress").toInt()) {
                proc->setProperty("lastProgress", percent);
                if (progress)
                    progress(percent, 100);
            }
        });
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [proc, path, url, callback = std::move(callback), progress](int exitCode,
                                                                            QProcess::ExitStatus) {
                    proc->deleteLater();
                    const QFileInfo info(path);
                    if (exitCode == 0 && info.exists() && info.size() > 0) {
                        if (progress)
                            progress(100, 100);
                        if (callback)
                            callback(true, {});
                        return;
                    }
                    const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
                    if (callback)
                        callback(false,
                                 QStringLiteral("下载失败（aria2 退出码 %1）：%2")
                                     .arg(exitCode)
                                     .arg(err.isEmpty() ? url.left(120) : err.left(200)));
                });
#ifdef Q_OS_WIN
        proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif
        // B站图片必须带 Referer，否则 403
        proc->setProgram(aria2);
        proc->setArguments({
            QStringLiteral("--dir=%1").arg(target.absolutePath()),
            QStringLiteral("--out=%1").arg(target.fileName()),
            QStringLiteral("--continue=true"),
            QStringLiteral("--allow-overwrite=true"),
            QStringLiteral("--auto-file-renaming=false"),
            QStringLiteral("--split=16"),
            QStringLiteral("--max-connection-per-server=16"),
            QStringLiteral("--min-split-size=1M"),
            QStringLiteral("--max-tries=3"),
            QStringLiteral("--retry-wait=2"),
            QStringLiteral("--connect-timeout=20"),
            QStringLiteral("--timeout=120"),
            QStringLiteral("--console-log-level=warn"),
            QStringLiteral("--summary-interval=1"),
            QStringLiteral("--show-console-readout=true"),
            QStringLiteral("--referer=https://www.bilibili.com/"),
            QStringLiteral("--user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                           "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"),
            url,
        });
        proc->start();
        return;
    }

    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(120000);
    // B站图片走 CDN，不带 Referer 会 403
    if (host.endsWith(QStringLiteral("hdslb.com")) || host.endsWith(QStringLiteral("biliimg.com"))) {
        request.setRawHeader("Referer", "https://www.bilibili.com/");
        request.setRawHeader("User-Agent",
                             "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                             "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
    }
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_manager->get(request);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [progress](qint64 received, qint64 total) {
                if (progress)
                    progress(received, total);
            });
    connect(reply, &QNetworkReply::finished, this, [reply, path, callback = std::move(callback)]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status > 299) {
            if (callback)
                callback(false,
                         QStringLiteral("下载失败（HTTP %1）：%2").arg(status).arg(reply->errorString()));
            return;
        }
        const QByteArray data = reply->readAll();
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (callback)
                callback(false, QStringLiteral("无法写入下载文件"));
            return;
        }
        file.write(data);
        file.close();
        if (callback)
            callback(true, {});
    });
}

void Backend::resolveFeijiiShare(const QString &shareUrl, UrlCallback callback)
{
    const QUrl source(shareUrl);
    if (!source.isValid() || !isFeijiiShareUrl(source)) {
        if (callback)
            callback(false, {}, {}, 0, QStringLiteral("小飞机永久分享链接格式无效"));
        return;
    }
    static const QRegularExpression sharePattern(QStringLiteral("/s/([^/?]+)"));
    const QRegularExpressionMatch match = sharePattern.match(source.path());
    const QString shareId = match.hasMatch() ? match.captured(1) : QString();
    if (shareId.isEmpty()) {
        if (callback)
            callback(false, {}, {}, 0, QStringLiteral("无法读取小飞机 shareId"));
        return;
    }

    QString cryptoError;
    const qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    const QString timestampCipher = feijiiEncryptParameter(QString::number(timestamp), &cryptoError);
    if (timestampCipher.isEmpty()) {
        if (callback)
            callback(false, {}, {}, 0, cryptoError);
        return;
    }
    const QString uuid = feijiiUuid();

    auto makeUrl = [uuid, timestampCipher](const QString &path, QUrlQuery query) {
        query.addQueryItem(QStringLiteral("devType"), QStringLiteral("6"));
        query.addQueryItem(QStringLiteral("devModel"), QStringLiteral("Chrome"));
        query.addQueryItem(QStringLiteral("uuid"), uuid);
        query.addQueryItem(QStringLiteral("extra"), QStringLiteral("2"));
        query.addQueryItem(QStringLiteral("timestamp"), timestampCipher);
        QUrl url(QStringLiteral("https://api.feijipan.com/ws/") + path);
        url.setQuery(query);
        return url;
    };

    QNetworkRequest vipRequest(makeUrl(QStringLiteral("buy/vip/list"), {}));
    configureFeijiiRequest(&vipRequest);
    vipRequest.setHeader(QNetworkRequest::ContentLengthHeader, 0);
    QNetworkReply *vipReply = m_manager->post(vipRequest, QByteArray());
    connect(vipReply, &QNetworkReply::finished, this,
            [this, vipReply, shareId, uuid, timestampCipher, callback = std::move(callback)]() mutable {
        vipReply->deleteLater();

        QUrlQuery query;
        query.addQueryItem(QStringLiteral("devType"), QStringLiteral("6"));
        query.addQueryItem(QStringLiteral("devModel"), QStringLiteral("Chrome"));
        query.addQueryItem(QStringLiteral("uuid"), uuid);
        query.addQueryItem(QStringLiteral("extra"), QStringLiteral("2"));
        query.addQueryItem(QStringLiteral("timestamp"), timestampCipher);
        query.addQueryItem(QStringLiteral("shareId"), shareId);
        query.addQueryItem(QStringLiteral("type"), QStringLiteral("0"));
        query.addQueryItem(QStringLiteral("offset"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("60"));
        QUrl url(QStringLiteral("https://api.feijipan.com/ws/recommend/list"));
        url.setQuery(query);
        QNetworkRequest request(url);
        configureFeijiiRequest(&request);
        request.setHeader(QNetworkRequest::ContentLengthHeader, 0);
        QNetworkReply *reply = m_manager->post(request, QByteArray());
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, shareId, uuid, callback = std::move(callback)]() mutable {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            const QString networkError = reply->errorString();
            const bool networkOk = reply->error() == QNetworkReply::NoError;
            reply->deleteLater();
            if (!networkOk || status < 200 || status > 299) {
                if (callback)
                    callback(false, {}, {}, 0,
                             QStringLiteral("读取小飞机分享信息失败（HTTP %1）：%2")
                                 .arg(status).arg(networkError));
                return;
            }

            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
            const QJsonObject root = doc.object();
            if (parseError.error != QJsonParseError::NoError || root.value(QStringLiteral("code")).toInt() != 200) {
                if (callback)
                    callback(false, {}, {}, 0,
                             root.value(QStringLiteral("msg")).toString(
                                 QStringLiteral("小飞机分享信息响应无法解析")));
                return;
            }
            const QJsonArray list = root.value(QStringLiteral("list")).toArray();
            const QJsonObject fileInfo = list.isEmpty() ? QJsonObject() : list.first().toObject();
            const QJsonArray details = fileInfo.value(QStringLiteral("fileList")).toArray();
            const QJsonObject detail = details.isEmpty() ? QJsonObject() : details.first().toObject();
            const QString fileId = jsonScalarText(fileInfo.value(QStringLiteral("fileIds")));
            QString userId = jsonScalarText(fileInfo.value(QStringLiteral("userId")));
            if (userId.isEmpty())
                userId = jsonScalarText(fileInfo.value(QStringLiteral("map")).toObject()
                                            .value(QStringLiteral("userId")));
            if (userId.isEmpty())
                userId = jsonScalarText(fileInfo.value(QStringLiteral("shareId")));
            if (fileId.isEmpty() || userId.isEmpty() || detail.isEmpty()) {
                if (callback)
                    callback(false, {}, {}, 0, QStringLiteral("小飞机分享中没有可下载文件"));
                return;
            }

            QString cryptoError;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const QString timestamp2 = feijiiEncryptParameter(QString::number(now), &cryptoError);
            const QString downloadId = feijiiEncryptParameter(fileId + QLatin1Char('|') + userId, &cryptoError);
            const QString auth = feijiiEncryptParameter(fileId + QLatin1Char('|') + QString::number(now), &cryptoError);
            if (timestamp2.isEmpty() || downloadId.isEmpty() || auth.isEmpty()) {
                if (callback)
                    callback(false, {}, {}, 0, cryptoError);
                return;
            }

            QUrlQuery redirectQuery;
            redirectQuery.addQueryItem(QStringLiteral("downloadId"), downloadId);
            redirectQuery.addQueryItem(QStringLiteral("enable"), QStringLiteral("1"));
            redirectQuery.addQueryItem(QStringLiteral("devType"), QStringLiteral("6"));
            redirectQuery.addQueryItem(QStringLiteral("uuid"), uuid);
            redirectQuery.addQueryItem(QStringLiteral("timestamp"), timestamp2);
            redirectQuery.addQueryItem(QStringLiteral("auth"), auth);
            redirectQuery.addQueryItem(QStringLiteral("shareId"), shareId);
            QUrl redirectUrl(QStringLiteral("https://api.feijipan.com/ws/file/redirect"));
            redirectUrl.setQuery(redirectQuery);
            QNetworkRequest redirectRequest(redirectUrl);
            configureFeijiiRequest(&redirectRequest);
            redirectRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                         QNetworkRequest::ManualRedirectPolicy);
            QNetworkReply *redirectReply = m_manager->get(redirectRequest);
            connect(redirectReply, &QNetworkReply::finished, this,
                    [redirectReply,
                     fileName = detail.value(QStringLiteral("fileName")).toString(),
                     fileSize = static_cast<qint64>(detail.value(QStringLiteral("fileSize")).toDouble()),
                     callback = std::move(callback)]() mutable {
                const int redirectStatus =
                    redirectReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QUrl target = redirectReply->attribute(
                    QNetworkRequest::RedirectionTargetAttribute).toUrl();
                const QString err = redirectReply->errorString();
                redirectReply->deleteLater();
                if (redirectStatus >= 300 && redirectStatus < 400 && target.isValid()) {
                    if (callback)
                        callback(true, target.toString(), fileName, fileSize, {});
                    return;
                }
                if (callback)
                    callback(false, {}, fileName, fileSize,
                             QStringLiteral("小飞机未返回下载直链（HTTP %1）：%2")
                                 .arg(redirectStatus).arg(err));
            });
        });
    });
}

} // namespace cj
