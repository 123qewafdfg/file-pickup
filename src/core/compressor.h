#pragma once

#include <QString>

#include <atomic>
#include <functional>

namespace cj {

// 源内容压缩：与安卓「隐像」保持同一套逻辑。
// 图片：静图一律转 WebP q75，不缩放，只有更小才采用（GIF / 动画 WebP 跳过）。
// 视频压缩已移除（用户决定不做），视频按原文件处理。
// 载体图（100x100 PNG）不经过本模块。

struct CompressOptions {
    bool images = true;
    int imageQuality = 75;    // WebP 质量
    int carrierQuality = 95;  // 首图封面 JPEG 质量
};

struct CompressResult {
    enum Status { Compressed, Skipped, Failed };

    Status status = Skipped;
    QString inputPath;
    QString outputPath;
    QString mime;
    QString method;           // "webp-q75" / "jpeg-q95" / 空
    QString error;
    QString note;             // 给界面显示的原因码
    qint64 originalBytes = 0;
    qint64 resultBytes = 0;
    QString originalSha256;
    QString resultSha256;

    bool adopted() const { return status == Compressed; }
};

class Compressor {
public:
    explicit Compressor(CompressOptions options = CompressOptions{});

    static bool isImage(const QString &path);
    static bool isAnimated(const QString &path);

    CompressResult compress(const QString &path,
                            const QString &workDir,
                            const std::function<void(int)> &progress = {},
                            const std::atomic_bool *cancel = nullptr) const;

    // 首图封面：JPEG q95，仅在更小时采用（安卓 prepareFirstCarrier 同款）
    CompressResult prepareCarrierCover(const QString &path, const QString &workDir) const;

    const CompressOptions &options() const { return m_options; }

private:
    CompressOptions m_options;
};

} // namespace cj
