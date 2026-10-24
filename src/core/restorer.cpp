#include "core/restorer.h"

#include "core/crypto.h"
#include "core/secretstream.h"
#include "core/tmshare.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace cj {

namespace {

QString safeFileName(const QString &raw)
{
    QString name = QFileInfo(raw).fileName();
    if (name.isEmpty())
        name = QStringLiteral("restored.bin");
    return name;
}

QByteArray inlineTextAad()
{
    return QByteArrayLiteral("TMSHARE2:INLINE"); // 16 字节
}

} // namespace

bool restoreFromCarriers(const QList<QString> &carrierPaths,
                         const ResourceRecord &record,
                         const QString &workDir,
                         RestoredResult *result,
                         QString *error)
{
    if (carrierPaths.isEmpty()) {
        if (error)
            *error = QStringLiteral("远端图片数量不完整");
        return false;
    }
    if (record.fileKey.size() != 32) {
        if (error)
            *error = QStringLiteral("口令记录中的密钥无效");
        return false;
    }
    if (!QDir().mkpath(workDir)) {
        if (error)
            *error = QStringLiteral("无法创建恢复目录");
        return false;
    }

    struct Loaded {
        CarrierIndex index;
        QString path;
    };
    QList<Loaded> loaded;
    QByteArray uuid;
    int partCount = 0;

    for (const QString &path : carrierPaths) {
        CarrierIndex index;
        if (!readCarrierIndex(path, &index, error))
            return false;
        if (uuid.isEmpty()) {
            uuid = index.uuid;
            partCount = index.partCount;
        } else if (index.uuid != uuid) {
            if (error)
                *error = QStringLiteral("载体分片不属于同一份内容");
            return false;
        }
        loaded.append({index, path});
    }

    if (loaded.size() != partCount) {
        if (error)
            *error = QStringLiteral("载体分片数量不完整（需要 %1 个，实际 %2 个）")
                         .arg(partCount)
                         .arg(loaded.size());
        return false;
    }

    std::sort(loaded.begin(), loaded.end(), [](const Loaded &a, const Loaded &b) {
        return a.index.partIndex < b.index.partIndex;
    });
    for (int i = 0; i < loaded.size(); ++i) {
        if (loaded.at(i).index.partIndex != i || loaded.at(i).index.partCount != partCount) {
            if (error)
                *error = QStringLiteral("载体分片序号重复或不连续");
            return false;
        }
    }

    // 元数据：优先用第 1 个分片自带的密文，否则用后端记录里的
    const QByteArray metaCipher = [&]() -> QByteArray {
        for (const Loaded &item : std::as_const(loaded)) {
            if (item.index.metaLen > 0) {
                QFile file(item.path);
                if (!file.open(QIODevice::ReadOnly))
                    return {};
                if (!file.seek(item.index.imageLen + item.index.cipherLen))
                    return {};
                return file.read(item.index.metaLen);
            }
        }
        return {};
    }();

    QByteArray metaIv;
    QByteArray metaPayload;
    if (!metaCipher.isEmpty()) {
        for (const Loaded &item : std::as_const(loaded)) {
            if (item.index.metaLen > 0) {
                metaIv = item.index.metaIv;
                break;
            }
        }
        metaPayload = metaCipher;
    } else {
        if (record.encryptedMetadata.size() <= kIvSize) {
            if (error)
                *error = QStringLiteral("口令记录缺少元数据");
            return false;
        }
        metaIv = record.encryptedMetadata.left(kIvSize);
        metaPayload = record.encryptedMetadata.mid(kIvSize);
    }

    const QByteArray metaJson =
        aesGcmDecrypt(record.fileKey, metaIv, metaPayload, metaAad(uuid), error);
    if (metaJson.isEmpty())
        return false;

    QJsonParseError parseError;
    const QJsonDocument metaDoc = QJsonDocument::fromJson(metaJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !metaDoc.isObject()) {
        if (error)
            *error = QStringLiteral("元数据无法解析");
        return false;
    }
    const QJsonObject meta = metaDoc.object();
    const QString originalName = safeFileName(meta.value(QStringLiteral("fileName")).toString());
    const QString mime = meta.value(QStringLiteral("mimeType"))
                             .toString(QStringLiteral("application/octet-stream"));
    const bool usesSecretStream = (loaded.first().index.flags & kCarrierFlagSecretStream) != 0;
    for (const Loaded &item : std::as_const(loaded)) {
        if (((item.index.flags & kCarrierFlagSecretStream) != 0) != usesSecretStream) {
            if (error)
                *error = QStringLiteral("载体分片加密格式不一致");
            return false;
        }
    }
    QByteArray streamState;
    if (usesSecretStream) {
        const QByteArray streamHeader = fromBase64Url(
            meta.value(QStringLiteral("streamHeader")).toString());
        if (!secretStreamInitPull(record.fileKey, streamHeader, &streamState, error))
            return false;
    }

    const QString partialPath = QDir(workDir).filePath(QStringLiteral(".restore-%1.part")
                                                           .arg(QDateTime::currentMSecsSinceEpoch()));
    QFile output(partialPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入恢复文件");
        return false;
    }

    for (const Loaded &item : std::as_const(loaded)) {
        QFile carrier(item.path);
        if (!carrier.open(QIODevice::ReadOnly)
            || !carrier.seek(item.index.imageLen)) {
            output.close();
            output.remove();
            if (error)
                *error = QStringLiteral("读取载体分片失败：%1").arg(QFileInfo(item.path).fileName());
            return false;
        }
        const QByteArray cipher = carrier.read(item.index.cipherLen);
        if (cipher.size() != item.index.cipherLen) {
            output.close();
            output.remove();
            if (error)
                *error = QStringLiteral("载体分片被截断");
            return false;
        }
        if (sha256(cipher) != item.index.cipherHash) {
            output.close();
            output.remove();
            if (error)
                *error = QStringLiteral("载体密文 SHA-256 校验失败");
            return false;
        }
        const QByteArray aad = dataAad(uuid, item.index.partIndex, item.index.partCount);
        QByteArray plain;
        if (usesSecretStream) {
            bool finalMessage = false;
            if (!secretStreamPull(&streamState, cipher, aad, &plain, &finalMessage, error)) {
                output.close();
                output.remove();
                return false;
            }
            const bool expectedFinal = item.index.partIndex + 1 == item.index.partCount;
            if (finalMessage != expectedFinal) {
                output.close();
                output.remove();
                if (error)
                    *error = QStringLiteral("secretstream 终止标记无效");
                return false;
            }
        } else {
            plain = aesGcmDecrypt(record.fileKey, item.index.iv, cipher, aad, error);
        }
        if (plain.isEmpty()) {
            output.close();
            output.remove();
            return false;
        }
        output.write(plain);
    }
    output.flush();
    output.close();

    const QByteArray actualSha = sha256File(partialPath);
    const QString expected = record.originalSha256Hex.trimmed().toLower();
    if (actualSha.isEmpty()
        || (!expected.isEmpty() && hex(actualSha) != expected)) {
        QFile::remove(partialPath);
        if (error)
            *error = QStringLiteral("恢复文件完整性校验失败");
        return false;
    }

    const QString finalPath = QDir(workDir).filePath(
        QStringLiteral("restored-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(originalName));
    if (!QFile::rename(partialPath, finalPath)) {
        QFile::remove(partialPath);
        if (error)
            *error = QStringLiteral("保存恢复文件失败");
        return false;
    }

    RestoredResult restored;
    restored.path = finalPath;
    restored.name = originalName;
    restored.mime = mime;

    const bool isBundle = meta.value(QStringLiteral("bundle")).toBool(false)
                          || originalName == QStringLiteral("hidden-files.zip");

    if (isBundle) {
        // 直接解到本次的提取目录，解完把中间产生的 zip 物理删除（QFile::remove 不进回收站）
        QList<BundleItem> children;
        if (!bundle::unpack(finalPath, workDir, &children, error))
            return false;
        if (!QFile::remove(finalPath)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("无法删除临时压缩包");
            return false;
        }
        restored.children = children;
        restored.path = workDir;
    }

    if (result)
        *result = restored;
    return true;
}

bool restoreInlineText(const ResourceRecord &record,
                       const QString &workDir,
                       RestoredResult *result,
                       QString *error)
{
    if (record.encryptedMetadata.size() <= kIvSize) {
        if (error)
            *error = QStringLiteral("文字口令记录不完整");
        return false;
    }
    const QByteArray iv = record.encryptedMetadata.left(kIvSize);
    const QByteArray plain = aesGcmDecrypt(record.fileKey,
                                           iv,
                                           record.encryptedMetadata.mid(kIvSize),
                                           inlineTextAad(),
                                           error);
    if (plain.isEmpty())
        return false;

    const QString expected = record.originalSha256Hex.trimmed().toLower();
    if (!expected.isEmpty() && hex(sha256(plain)) != expected) {
        if (error)
            *error = QStringLiteral("文字内容完整性校验失败");
        return false;
    }

    if (!QDir().mkpath(workDir)) {
        if (error)
            *error = QStringLiteral("无法创建恢复目录");
        return false;
    }
    const QString path = QDir(workDir).filePath(QStringLiteral("提取文字.txt"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入提取结果");
        return false;
    }
    file.write(plain);
    file.close();

    if (result) {
        result->path = path;
        result->name = QStringLiteral("提取文字.txt");
        result->mime = QStringLiteral("text/plain");
        result->children.clear();
    }
    return true;
}

} // namespace cj
