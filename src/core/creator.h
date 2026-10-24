#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace cj {

struct CarrierPart {
    QString path;              // 载体 PNG 绝对路径
    QString remoteSha256Hex;   // 整个文件的 SHA-256
    QString cipherSha256Hex;   // 密文段 SHA-256
    qint64 cipherSize = 0;     // 含 16 字节 tag
    int partIndex = 0;
};

struct CreatedResource {
    QByteArray uuid;                 // 16 字节
    QByteArray fileKey;              // 32 字节
    QByteArray encryptedMetadata;    // 12 字节 iv + 密文
    QString originalSha256Hex;
    qint64 originalSize = 0;
    QList<CarrierPart> parts;
};

// QQ 流式载体计划：只保存生成单片所需的固定信息，不预先生成全部载体。
// 调用方可以 generateQqCarrierPart() → 上传 → 删除本地文件 → 继续下一片。
struct QqCarrierPlan {
    QString payloadPath;
    QString workDir;
    QByteArray coverImage;
    QByteArray metaIv;
    QByteArray streamState;
    QByteArray streamHeader;
    QList<qint64> plainSizes;
    bool binaryMode = false;        // 小飞机：无图片外壳，输出 .cjpart
    CreatedResource resource;
};

bool prepareQqCarrierPlan(const QString &payloadPath,
                          const QString &displayName,
                          const QString &mimeType,
                          const QByteArray &coverImage,
                          const QString &workDir,
                          QqCarrierPlan *plan,
                          QString *error,
                          bool bundle = false,
                          bool binaryMode = false);

bool generateQqCarrierPart(QqCarrierPlan *plan,
                           int index,
                           CarrierPart *part,
                           QString *error);

// 把一段内容加密并追加到承载图尾部，产出 N 张载体 PNG。
// qqMode = true 时使用 29 MiB 分片规则，false 时使用 B站规则。
bool generateCarriers(const QString &payloadPath,
                      const QString &displayName,
                      const QString &mimeType,
                      const QByteArray &coverPng,
                      const QString &workDir,
                      bool qqMode,
                      CreatedResource *resource,
                      QString *error,
                      bool bundle = false);

} // namespace cj
