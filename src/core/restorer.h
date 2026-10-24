#pragma once

#include "core/payload.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace cj {

struct ResourcePart {
    int partIndex = 0;
    QString downloadUrl;
    QString remoteSha256Hex;
    QString cipherSha256Hex;
    qint64 cipherSize = 0;
};

// 后端口令记录（tm-get 返回的内容）
struct ResourceRecord {
    int protocolVersion = 1;
    QByteArray fileKey;             // 32 字节
    QByteArray encryptedMetadata;   // 12 字节 iv + 密文
    QString originalSha256Hex;
    QList<ResourcePart> parts;
};

struct RestoredResult {
    QString path;     // 恢复出的本地文件
    QString name;     // 原始文件名
    QString mime;
    QList<BundleItem> children; // 多文件包解包结果（单文件为空）
};

// 从已下载到本地的载体分片恢复内容（分片顺序无关，按索引排序）
bool restoreFromCarriers(const QList<QString> &carrierPaths,
                         const ResourceRecord &record,
                         const QString &workDir,
                         RestoredResult *result,
                         QString *error);

// 纯文字口令（protocolVersion = 2）直接解出文本
bool restoreInlineText(const ResourceRecord &record,
                       const QString &workDir,
                       RestoredResult *result,
                       QString *error);

} // namespace cj
