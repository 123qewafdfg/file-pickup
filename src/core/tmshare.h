#pragma once

#include <QByteArray>
#include <QString>

namespace cj {

// TMSHARE1 载体格式（与安卓版逐字节一致）
//
// 文件布局：PNG 原图 || AES-GCM 密文（含 16 字节 tag）|| 元数据（仅第 1 个分片）|| 112 字节索引
//
// 索引（大端）：
//   0..7    "TMSHARE1"
//   8..9    版本 = 1
//   10..11  flags：bit 0 = libsodium secretstream
//   12..27  uuid（16 字节）
//   28..29  分片序号 partIndex
//   30..31  分片总数 partCount
//   32..39  密文长度 cipherLen（含 tag）
//   40..43  元数据长度 metaLen
//   44..51  原图长度 imageLen
//   52..63  nonce iv（12 字节）
//   64..75  元数据 nonce metaIv（12 字节）
//   76..107 密文 SHA-256（32 字节）
//   108..111 前 108 字节的 CRC32

constexpr int kIndexSize = 112;
constexpr int kIvSize = 12;
constexpr int kTagSize = 16;
constexpr quint16 kCarrierFlagSecretStream = 0x0001;

struct CarrierIndex {
    QByteArray uuid;        // 16 字节
    quint16 flags = 0;
    quint16 partIndex = 0;
    quint16 partCount = 1;
    qint64 cipherLen = 0;   // 含 16 字节 tag
    qint32 metaLen = 0;
    qint64 imageLen = 0;
    QByteArray iv;          // 12 字节
    QByteArray metaIv;      // 12 字节
    QByteArray cipherHash;  // 32 字节
};

QByteArray buildIndex(const CarrierIndex &index);

// 从文件尾部读取并校验索引
bool readCarrierIndex(const QString &path, CarrierIndex *index, QString *error);

// PNG 结束块（IEND 之后）的精确长度；用于确认载体图本身不含尾部数据
qint64 cleanPngLength(const QByteArray &png);

// 载体基础图的精确长度；支持 PNG 与 JPEG，拒绝本身已经带尾部数据的图片。
qint64 cleanCarrierImageLength(const QByteArray &image);

// 与安卓一致的 AAD：DATA 为 36 字节（含 4 字节隐式补零），META 为 29 字节
QByteArray dataAad(const QByteArray &uuid, quint16 partIndex, quint16 partCount);
QByteArray metaAad(const QByteArray &uuid);

// 生成 100x100 纯白 PNG（QQ 频道承载图，与安卓一致）
QByteArray whiteQqCarrierPng();

// B站固定承载图（由隐像 nativeAssets/bili_carrier.png 内置而来）。
// B 站 CDN 会重编码小图/纯色图，只有这张 1920x1280 的图追加数据后能原样取回。
QByteArray biliCarrierPng();

} // namespace cj
