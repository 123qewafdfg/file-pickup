#include "core/creator.h"

#include "core/crypto.h"
#include "core/partition.h"
#include "core/secretstream.h"
#include "core/tmshare.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace cj {

namespace {

QByteArray uuidHex(const QByteArray &uuid)
{
    return uuid.toHex();
}

QByteArray buildMetadata(const QString &displayName,
                         const QString &mimeType,
                         qint64 payloadSize,
                         const QString &originalSha256Hex,
                         bool bundle,
                         const QByteArray &streamHeader = {})
{
    QJsonObject meta;
    meta.insert(QStringLiteral("version"), streamHeader.isEmpty() ? 1 : 2);
    meta.insert(QStringLiteral("fileName"), displayName);
    meta.insert(QStringLiteral("mimeType"), mimeType);
    meta.insert(QStringLiteral("originalSize"), static_cast<double>(payloadSize));
    meta.insert(QStringLiteral("originalSha256"), originalSha256Hex);
    if (bundle)
        meta.insert(QStringLiteral("bundle"), true);
    if (!streamHeader.isEmpty()) {
        meta.insert(QStringLiteral("cipher"), QStringLiteral("secretstream-xchacha20poly1305"));
        meta.insert(QStringLiteral("streamHeader"), base64Url(streamHeader));
    }
    return QJsonDocument(meta).toJson(QJsonDocument::Compact);
}

} // namespace

bool prepareQqCarrierPlan(const QString &payloadPath,
                          const QString &displayName,
                          const QString &mimeType,
                          const QByteArray &coverImage,
                          const QString &workDir,
                          QqCarrierPlan *plan,
                          QString *error,
                          bool bundle,
                          bool binaryMode)
{
    if (!plan)
        return false;

    QFileInfo payloadInfo(payloadPath);
    const qint64 payloadSize = payloadInfo.size();
    if (!payloadInfo.isFile() || payloadSize <= 0) {
        if (error)
            *error = QStringLiteral("隐藏内容为空或不存在");
        return false;
    }
    if (!binaryMode && cleanCarrierImageLength(coverImage) != coverImage.size()) {
        if (error)
            *error = QStringLiteral("承载图无效（仅支持无尾部数据的 PNG/JPEG）");
        return false;
    }

    QqCarrierPlan result;
    result.payloadPath = payloadPath;
    result.workDir = workDir;
    result.coverImage = coverImage;
    result.binaryMode = binaryMode;
    if (binaryMode) {
        constexpr qint64 kCloudPartBytes = 64LL * 1024 * 1024;
        const qint64 partCount = (payloadSize + kCloudPartBytes - 1) / kCloudPartBytes;
        if (partCount > 0 && partCount <= 65535) {
            result.plainSizes.reserve(static_cast<int>(partCount));
            qint64 remaining = payloadSize;
            while (remaining > 0) {
                const qint64 size = qMin(remaining, kCloudPartBytes);
                result.plainSizes.append(size);
                remaining -= size;
            }
        }
    } else {
        result.plainSizes = qq::plan(payloadSize);
    }
    if (result.plainSizes.isEmpty()) {
        if (error)
            *error = QStringLiteral("内容过大，超过载体协议分片数量上限");
        return false;
    }

    result.resource.uuid = randomBytes(16);
    result.resource.fileKey = randomBytes(32);
    result.resource.originalSize = payloadSize;
    const QByteArray originalSha = sha256File(payloadPath, error);
    if (originalSha.isEmpty())
        return false;
    result.resource.originalSha256Hex = hex(originalSha);

    if (binaryMode
        && !secretStreamInitPush(result.resource.fileKey,
                                 &result.streamState,
                                 &result.streamHeader,
                                 error)) {
        return false;
    }

    result.metaIv = randomBytes(kIvSize);
    const QByteArray metaJson = buildMetadata(displayName,
                                              mimeType,
                                              payloadSize,
                                              result.resource.originalSha256Hex,
                                              bundle,
                                              result.streamHeader);
    const QByteArray encryptedMeta = aesGcmEncrypt(result.resource.fileKey,
                                                   result.metaIv,
                                                   metaJson,
                                                   metaAad(result.resource.uuid),
                                                   error);
    if (encryptedMeta.isEmpty())
        return false;
    result.resource.encryptedMetadata = result.metaIv + encryptedMeta;

    const QString dir = QDir(workDir).filePath(QString::fromLatin1(uuidHex(result.resource.uuid)));
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("无法创建流式载体缓存目录");
        return false;
    }

    *plan = result;
    return true;
}

bool generateQqCarrierPart(QqCarrierPlan *plan,
                           int index,
                           CarrierPart *part,
                           QString *error)
{
    if (!plan || !part || index < 0 || index >= plan->plainSizes.size()) {
        if (error)
            *error = QStringLiteral("载体分片序号无效");
        return false;
    }

    qint64 offset = 0;
    for (int i = 0; i < index; ++i)
        offset += plan->plainSizes.at(i);

    QFile payload(plan->payloadPath);
    if (!payload.open(QIODevice::ReadOnly) || !payload.seek(offset)) {
        if (error)
            *error = QStringLiteral("无法读取待处理内容分片");
        return false;
    }
    const qint64 plainSize = plan->plainSizes.at(index);
    const QByteArray plain = payload.read(plainSize);
    if (plain.size() != plainSize) {
        if (error)
            *error = QStringLiteral("读取内容分片失败");
        return false;
    }

    const QByteArray carrierImage = plan->binaryMode
                                        ? QByteArray()
                                        : (index == 0 ? plan->coverImage : whiteQqCarrierPng());
    const QByteArray aad = dataAad(plan->resource.uuid,
                                   static_cast<quint16>(index),
                                   static_cast<quint16>(plan->plainSizes.size()));
    QByteArray iv(kIvSize, '\0');
    QByteArray cipher;
    if (plan->binaryMode) {
        if (!secretStreamPush(&plan->streamState,
                              plain,
                              aad,
                              index + 1 == plan->plainSizes.size(),
                              &cipher,
                              error)) {
            return false;
        }
    } else {
        iv = randomBytes(kIvSize);
        cipher = aesGcmEncrypt(plan->resource.fileKey, iv, plain, aad, error);
        if (cipher.isEmpty())
            return false;
    }

    const QByteArray metaPart = index == 0 ? plan->resource.encryptedMetadata.mid(kIvSize)
                                           : QByteArray();
    CarrierIndex carrierIndex;
    carrierIndex.uuid = plan->resource.uuid;
    carrierIndex.flags = plan->binaryMode ? kCarrierFlagSecretStream : 0;
    carrierIndex.partIndex = static_cast<quint16>(index);
    carrierIndex.partCount = static_cast<quint16>(plan->plainSizes.size());
    carrierIndex.cipherLen = cipher.size();
    carrierIndex.metaLen = metaPart.size();
    carrierIndex.imageLen = carrierImage.size();
    carrierIndex.iv = iv;
    carrierIndex.metaIv = plan->metaIv;
    carrierIndex.cipherHash = sha256(cipher);

    const bool jpegCover = carrierImage.size() >= 2
                           && static_cast<quint8>(carrierImage.at(0)) == 0xFF
                           && static_cast<quint8>(carrierImage.at(1)) == 0xD8;
    const QString dir = QDir(plan->workDir).filePath(QString::fromLatin1(uuidHex(plan->resource.uuid)));
    const QString path = QDir(dir).filePath(
        QStringLiteral("%1-%2.%3")
            .arg(QString::fromLatin1(uuidHex(plan->resource.uuid)))
            .arg(index + 1)
            .arg(plan->binaryMode ? QStringLiteral("cjpart")
                                 : jpegCover ? QStringLiteral("jpg") : QStringLiteral("png")));
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入流式载体：%1").arg(path);
        return false;
    }
    out.write(carrierImage);
    out.write(cipher);
    out.write(metaPart);
    out.write(buildIndex(carrierIndex));
    out.close();

    CarrierPart generated;
    generated.path = path;
    generated.cipherSha256Hex = hex(carrierIndex.cipherHash);
    generated.cipherSize = cipher.size();
    generated.partIndex = index;
    generated.remoteSha256Hex = hex(sha256File(path));
    if (generated.remoteSha256Hex.isEmpty()) {
        if (error)
            *error = QStringLiteral("无法校验流式载体 SHA-256");
        return false;
    }
    *part = generated;
    return true;
}

bool generateCarriers(const QString &payloadPath,
                      const QString &displayName,
                      const QString &mimeType,
                      const QByteArray &coverPng,
                      const QString &workDir,
                      bool qqMode,
                      CreatedResource *resource,
                      QString *error,
                      bool bundle)
{
    if (!resource)
        return false;

    QFile payload(payloadPath);
    if (!payload.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取待处理内容：%1").arg(payloadPath);
        return false;
    }
    const qint64 payloadSize = payload.size();
    if (payloadSize <= 0) {
        if (error)
            *error = QStringLiteral("隐藏内容不能为空");
        return false;
    }

    if (cleanCarrierImageLength(coverPng) != coverPng.size()) {
        if (error)
            *error = QStringLiteral("承载图无效（仅支持无尾部数据的 PNG/JPEG）");
        return false;
    }

    CreatedResource result;
    result.uuid = randomBytes(16);
    result.fileKey = randomBytes(32);
    result.originalSize = payloadSize;

    const QByteArray originalSha = sha256File(payloadPath);
    if (originalSha.isEmpty()) {
        if (error)
            *error = QStringLiteral("无法计算原文件 SHA-256");
        return false;
    }
    result.originalSha256Hex = hex(originalSha);

    const QByteArray metaJson = buildMetadata(displayName,
                                              mimeType,
                                              payloadSize,
                                              result.originalSha256Hex,
                                              bundle);

    const QByteArray metaIv = randomBytes(kIvSize);
    const QByteArray encryptedMeta =
        aesGcmEncrypt(result.fileKey, metaIv, metaJson, metaAad(result.uuid), error);
    if (encryptedMeta.isEmpty())
        return false;
    result.encryptedMetadata = metaIv + encryptedMeta;

    QList<qint64> sizes;
    if (qqMode)
        sizes = qq::plan(payloadSize);
    else
        sizes = bili::plan(payloadSize, encryptedMeta.size(), coverPng.size());

    if (sizes.isEmpty()) {
        if (error)
            *error = QStringLiteral("内容过大，无法生成载体分片");
        return false;
    }

    const QString dir = QDir(workDir).filePath(QString::fromLatin1(uuidHex(result.uuid)));
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("无法创建输出目录");
        return false;
    }

    const quint16 partCount = static_cast<quint16>(sizes.size());
    for (int index = 0; index < sizes.size(); ++index) {
        // QQ 只把用户首张内容图作为第一张封面；后续分片保持轻量白色 PNG。
        // B站必须每张都使用固定预设图，否则 CDN 会重编码并破坏尾部数据。
        const QByteArray carrierImage = (qqMode && index > 0) ? whiteQqCarrierPng() : coverPng;
        const qint64 size = sizes.at(index);
        const QByteArray plain = payload.read(size);
        if (plain.size() != size) {
            if (error)
                *error = QStringLiteral("读取内容失败");
            return false;
        }

        const QByteArray iv = randomBytes(kIvSize);
        const QByteArray cipher = aesGcmEncrypt(result.fileKey,
                                                iv,
                                                plain,
                                                dataAad(result.uuid,
                                                        static_cast<quint16>(index),
                                                        partCount),
                                                error);
        if (cipher.isEmpty())
            return false;

        const QByteArray metaPart = (index == 0) ? encryptedMeta : QByteArray();

        CarrierIndex carrierIndex;
        carrierIndex.uuid = result.uuid;
        carrierIndex.partIndex = static_cast<quint16>(index);
        carrierIndex.partCount = partCount;
        carrierIndex.cipherLen = cipher.size();
        carrierIndex.metaLen = metaPart.size();
        carrierIndex.imageLen = carrierImage.size();
        carrierIndex.iv = iv;
        carrierIndex.metaIv = metaIv;
        carrierIndex.cipherHash = sha256(cipher);
        const QByteArray indexBytes = buildIndex(carrierIndex);

        const bool jpegCover = carrierImage.size() >= 2
                               && static_cast<quint8>(carrierImage.at(0)) == 0xFF
                               && static_cast<quint8>(carrierImage.at(1)) == 0xD8;
        const QString path = QDir(dir).filePath(
            QStringLiteral("%1-%2.%3")
                .arg(QString::fromLatin1(uuidHex(result.uuid)))
                .arg(index + 1)
                .arg(jpegCover ? QStringLiteral("jpg") : QStringLiteral("png")));

        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("无法写入载体文件：%1").arg(path);
            return false;
        }
        out.write(carrierImage);
        out.write(cipher);
        out.write(metaPart);
        out.write(indexBytes);
        out.flush();
        out.close();

        CarrierPart part;
        part.path = path;
        part.cipherSha256Hex = hex(carrierIndex.cipherHash);
        part.cipherSize = cipher.size();
        part.partIndex = index;
        const QByteArray fileSha = sha256File(path);
        if (fileSha.isEmpty()) {
            if (error)
                *error = QStringLiteral("无法计算载体文件 SHA-256");
            return false;
        }
        part.remoteSha256Hex = hex(fileSha);
        result.parts.append(part);
    }

    *resource = result;
    return true;
}

} // namespace cj
