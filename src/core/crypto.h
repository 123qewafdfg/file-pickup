#pragma once

#include <QByteArray>
#include <QString>

namespace cj {

// 密码学基础原语：Windows CNG（BCrypt）实现，不依赖 OpenSSL。
// 与安卓版保持逐字节一致：AES-256-GCM 密文尾部带 16 字节认证标签，
// Base64 使用 URL_SAFE + 无填充，哈希为 SHA-256。

QByteArray randomBytes(int count);

QByteArray sha256(const QByteArray &data);
QByteArray sha256File(const QString &path, QString *error = nullptr);

// 返回 密文 || 16 字节 tag；失败返回空数组
QByteArray aesGcmEncrypt(const QByteArray &key,
                         const QByteArray &iv,
                         const QByteArray &plain,
                         const QByteArray &aad,
                         QString *error = nullptr);
QByteArray aesGcmDecrypt(const QByteArray &key,
                         const QByteArray &iv,
                         const QByteArray &cipherWithTag,
                         const QByteArray &aad,
                         QString *error = nullptr);

// 小飞机公开分享接口使用的兼容算法：AES/ECB/PKCS5Padding（等价 PKCS7）。
QByteArray aesEcbEncryptPkcs7(const QByteArray &key,
                              const QByteArray &plain,
                              QString *error = nullptr);
QByteArray aesEcbDecryptPkcs7(const QByteArray &key,
                              const QByteArray &cipher,
                              QString *error = nullptr);

quint32 crc32(const QByteArray &data);
quint32 crc32Update(quint32 seed, const char *data, qsizetype size);

QString base64Url(const QByteArray &data);
QByteArray fromBase64Url(const QString &text);

QString hex(const QByteArray &data);
QByteArray fromHex(const QString &text);

} // namespace cj
