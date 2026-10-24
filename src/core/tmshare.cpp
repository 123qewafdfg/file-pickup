#include "core/tmshare.h"

#include "core/crypto.h"

#include <QBuffer>
#include <QFile>
#include <QImage>

#include <cstring>

namespace cj {

namespace {

void putU16(char *p, quint16 v)
{
    p[0] = static_cast<char>((v >> 8) & 0xFF);
    p[1] = static_cast<char>(v & 0xFF);
}

void putU32(char *p, quint32 v)
{
    p[0] = static_cast<char>((v >> 24) & 0xFF);
    p[1] = static_cast<char>((v >> 16) & 0xFF);
    p[2] = static_cast<char>((v >> 8) & 0xFF);
    p[3] = static_cast<char>(v & 0xFF);
}

void putU64(char *p, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<char>((v >> (56 - 8 * i)) & 0xFF);
}

quint16 getU16(const char *p)
{
    return static_cast<quint16>((static_cast<quint8>(p[0]) << 8) | static_cast<quint8>(p[1]));
}

quint32 getU32(const char *p)
{
    return (static_cast<quint32>(static_cast<quint8>(p[0])) << 24)
        | (static_cast<quint32>(static_cast<quint8>(p[1])) << 16)
        | (static_cast<quint32>(static_cast<quint8>(p[2])) << 8)
        | static_cast<quint32>(static_cast<quint8>(p[3]));
}

quint64 getU64(const char *p)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<quint8>(p[i]);
    return value;
}

} // namespace

QByteArray buildIndex(const CarrierIndex &index)
{
    QByteArray raw(kIndexSize, '\0');
    char *p = raw.data();

    std::memcpy(p, "TMSHARE1", 8);
    putU16(p + 8, 1);
    putU16(p + 10, index.flags);
    std::memcpy(p + 12, index.uuid.constData(), 16);
    putU16(p + 28, index.partIndex);
    putU16(p + 30, index.partCount);
    putU64(p + 32, static_cast<quint64>(index.cipherLen));
    putU32(p + 40, static_cast<quint32>(index.metaLen));
    putU64(p + 44, static_cast<quint64>(index.imageLen));
    std::memcpy(p + 52, index.iv.constData(), kIvSize);
    std::memcpy(p + 64, index.metaIv.constData(), kIvSize);
    std::memcpy(p + 76, index.cipherHash.constData(), 32);
    putU32(p + 108, crc32(raw.left(108)));
    return raw;
}

bool readCarrierIndex(const QString &path, CarrierIndex *index, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取文件：%1").arg(path);
        return false;
    }
    if (file.size() < kIndexSize) {
        if (error)
            *error = QStringLiteral("文件过小，不是文件取件载体");
        return false;
    }
    if (!file.seek(file.size() - kIndexSize)) {
        if (error)
            *error = QStringLiteral("定位载体索引失败");
        return false;
    }
    const QByteArray raw = file.read(kIndexSize);
    if (raw.size() != kIndexSize) {
        if (error)
            *error = QStringLiteral("读取载体索引失败");
        return false;
    }

    const char *p = raw.constData();
    if (std::memcmp(p, "TMSHARE1", 8) != 0) {
        if (error)
            *error = QStringLiteral("该文件不是文件取件载体（缺少 TMSHARE1 标记）");
        return false;
    }
    const quint16 version = getU16(p + 8);
    if (version != 1) {
        if (error)
            *error = QStringLiteral("不支持的载体版本：%1").arg(version);
        return false;
    }
    const quint32 storedCrc = getU32(p + 108);
    if (crc32(raw.left(108)) != storedCrc) {
        if (error)
            *error = QStringLiteral("载体索引校验失败（CRC32 不一致）");
        return false;
    }

    CarrierIndex parsed;
    parsed.uuid = raw.mid(12, 16);
    parsed.flags = getU16(p + 10);
    if ((parsed.flags & ~kCarrierFlagSecretStream) != 0) {
        if (error)
            *error = QStringLiteral("载体使用了不支持的加密标志");
        return false;
    }
    parsed.partIndex = getU16(p + 28);
    parsed.partCount = getU16(p + 30);
    parsed.cipherLen = static_cast<qint64>(getU64(p + 32));
    parsed.metaLen = static_cast<qint32>(getU32(p + 40));
    parsed.imageLen = static_cast<qint64>(getU64(p + 44));
    parsed.iv = raw.mid(52, kIvSize);
    parsed.metaIv = raw.mid(64, kIvSize);
    parsed.cipherHash = raw.mid(76, 32);

    const qint64 expected = parsed.imageLen + parsed.cipherLen + parsed.metaLen + kIndexSize;
    // 小飞机网盘使用无图片外壳的 .cjpart，imageLen 合法值可以为 0。
    if (parsed.imageLen < 0 || parsed.cipherLen < kTagSize || parsed.metaLen < 0
        || expected != file.size()) {
        if (error)
            *error = QStringLiteral("载体结构不完整（长度不一致）");
        return false;
    }
    if (index)
        *index = parsed;
    return true;
}

qint64 cleanPngLength(const QByteArray &png)
{
    if (png.size() < 8)
        return -1;
    static const char signature[8] = {static_cast<char>(0x89), 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (std::memcmp(png.constData(), signature, 8) != 0)
        return -1;

    qint64 pos = 8;
    while (pos + 12 <= png.size()) {
        const quint32 length = getU32(png.constData() + pos);
        const char *type = png.constData() + pos + 4;
        pos += 12 + static_cast<qint64>(length);
        if (std::memcmp(type, "IEND", 4) == 0)
            return pos <= png.size() ? pos : -1;
    }
    return -1;
}

qint64 cleanCarrierImageLength(const QByteArray &image)
{
    const qint64 pngLength = cleanPngLength(image);
    if (pngLength >= 0)
        return pngLength == image.size() ? pngLength : -1;

    // JPEG 扫描数据内部的 0xFF 会做字节填充，完整文件应以 SOI 开始、EOI 结束。
    // 这里要求 EOI 正好位于文件末尾，从而拒绝已经追加过数据的图片。
    if (image.size() >= 4
        && static_cast<quint8>(image.at(0)) == 0xFF
        && static_cast<quint8>(image.at(1)) == 0xD8
        && static_cast<quint8>(image.at(image.size() - 2)) == 0xFF
        && static_cast<quint8>(image.at(image.size() - 1)) == 0xD9) {
        return image.size();
    }
    return -1;
}

QByteArray dataAad(const QByteArray &uuid, quint16 partIndex, quint16 partCount)
{
    // 安卓用 ByteBuffer.allocate(36) 后只写入 32 字节，尾部 4 字节为 0
    QByteArray aad(36, '\0');
    std::memcpy(aad.data(), "TMSHARE1:DATA", 12);
    std::memcpy(aad.data() + 12, uuid.constData(), 16);
    putU16(aad.data() + 28, partIndex);
    putU16(aad.data() + 30, partCount);
    return aad;
}

QByteArray metaAad(const QByteArray &uuid)
{
    QByteArray aad(13 + 16, '\0');
    std::memcpy(aad.data(), "TMSHARE1:META", 13);
    std::memcpy(aad.data() + 13, uuid.constData(), 16);
    return aad;
}

QByteArray whiteQqCarrierPng()
{
    QImage image(100, 100, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    buffer.close();
    return png;
}

QByteArray biliCarrierPng()
{
    QFile file(QStringLiteral(":/carrier/bili_carrier.png"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace cj
