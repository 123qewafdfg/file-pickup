#pragma once

#include <QList>

namespace cj {

// 与安卓版一致的分片规划

namespace qq {
constexpr qint64 kAppendBytes = 29LL * 1024 * 1024; // 每张图尾部最多 29 MiB
constexpr qint64 kPerImageOverhead = 128;            // AES-GCM tag 16 B + 索引 112 B
constexpr qint64 kPayloadBytesPerImage = kAppendBytes - kPerImageOverhead;
constexpr int kImagesPerPost = 18;
constexpr qint64 kBytesPerPost = kPayloadBytesPerImage * kImagesPerPost;
QList<qint64> plan(qint64 bytes);
}

namespace bili {
constexpr qint64 kMaxPayload = 100LL * 1024 * 1024;
constexpr int kMaxParts = 64;
constexpr qint64 kMaxCarrierBytes = 4'800'000;
constexpr qint64 kMaxAppend = 6LL * 1024 * 1024;   // 单张最多追加 6 MiB（与安卓一致）
constexpr qint64 kOverhead = 128;
QList<qint64> plan(qint64 bytes, int metadataBytes, qint64 carrierBytes);
}

} // namespace cj
