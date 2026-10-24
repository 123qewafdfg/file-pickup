#include "core/compressor.h"

#include "core/crypto.h"
#include "core/logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QUuid>

#include <algorithm>

namespace cj {
namespace {

constexpr int kCarrierCoverQuality = 95;

QStringList imageSuffixes()
{
    return {QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
            QStringLiteral("webp"), QStringLiteral("bmp"),  QStringLiteral("gif"),
            QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("heic")};
}

QString lowerSuffix(const QString &path)
{
    return QFileInfo(path).suffix().toLower();
}

bool webpWritable()
{
    static const bool supported = [] {
        const QList<QByteArray> formats = QImageWriter::supportedImageFormats();
        return formats.contains(QByteArrayLiteral("webp"));
    }();
    return supported;
}

qint64 fileSize(const QString &path)
{
    return QFileInfo(path).size();
}

void report(const std::function<void(int)> &progress, int percent)
{
    if (progress)
        progress(percent);
}

} // namespace

Compressor::Compressor(CompressOptions options) : m_options(std::move(options)) {}

bool Compressor::isImage(const QString &path)
{
    return imageSuffixes().contains(lowerSuffix(path));
}

bool Compressor::isAnimated(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray header = file.read(21);
    if (header.size() >= 6) {
        const QByteArray signature = header.left(6);
        if (signature == QByteArrayLiteral("GIF87a") || signature == QByteArrayLiteral("GIF89a"))
            return true;
    }
    if (header.size() >= 21 && header.left(4) == QByteArrayLiteral("RIFF")
        && header.mid(8, 4) == QByteArrayLiteral("WEBP") && header.mid(12, 4) == QByteArrayLiteral("VP8X")) {
        return (header.at(20) & 0x02) != 0;
    }
    return false;
}

CompressResult Compressor::compress(const QString &path,
                                    const QString &workDir,
                                    const std::function<void(int)> &progress,
                                    const std::atomic_bool *cancel) const
{
    CompressResult result;
    result.inputPath = path;
    result.outputPath = path;
    result.originalBytes = result.resultBytes = fileSize(path);

    if (!QFileInfo::exists(path)) {
        result.status = CompressResult::Failed;
        result.error = QStringLiteral("input-missing");
        return result;
    }

    if (!isImage(path)) {
        result.status = CompressResult::Skipped;
        result.note = QStringLiteral("unsupported-type");
        return result;
    }
    if (!m_options.images) {
        result.status = CompressResult::Skipped;
        result.note = QStringLiteral("images-disabled");
        return result;
    }
    if (isAnimated(path)) {
        result.status = CompressResult::Skipped;
        result.note = QStringLiteral("animated");
        return result;
    }
    if (!webpWritable()) {
        result.status = CompressResult::Failed;
        result.error = QStringLiteral("webp-encoder-missing");
        return result;
    }
    if (cancel != nullptr && cancel->load()) {
        result.status = CompressResult::Failed;
        result.error = QStringLiteral("cancelled");
        return result;
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        result.status = CompressResult::Failed;
        result.error = QStringLiteral("decode-failed");
        return result;
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        result.status = CompressResult::Failed;
        result.error = QStringLiteral("decode-failed");
        return result;
    }
    report(progress, 30);

    QDir().mkpath(workDir);
    const QString originalKey = hex(sha256File(path)).left(16);
    const QString candidate = QDir(workDir).filePath(
        QStringLiteral("%1-q%2.webp").arg(originalKey).arg(m_options.imageQuality));
    const QString temporary = QDir(workDir).filePath(
        QStringLiteral("%1-%2.tmp").arg(originalKey, QUuid::createUuid().toString(QUuid::Id128)));

    // Windows 上文件句柄未释放会导致后续 rename 失败，写入与校验都要限定作用域
    bool written = false;
    QString writeError;
    {
        QImageWriter writer(temporary, QByteArrayLiteral("webp"));
        writer.setQuality(std::clamp(m_options.imageQuality, 0, 100));
        written = writer.write(image);
        if (!written)
            writeError = writer.errorString();
    }
    report(progress, 70);

    if (!written || fileSize(temporary) <= 0) {
        QFile::remove(temporary);
        result.status = CompressResult::Failed;
        result.error = QStringLiteral("encode-failed: %1").arg(writeError);
        return result;
    }

    // L0：产物必须能重新解码，且像素尺寸不变（隐像不缩放）
    bool verified = false;
    {
        QImageReader verify(temporary);
        const QImage decoded = verify.read();
        verified = !decoded.isNull() && decoded.size() == image.size();
    }
    if (!verified) {
        QFile::remove(temporary);
        result.status = CompressResult::Failed;
        result.error = QStringLiteral("verify-failed");
        return result;
    }

    const qint64 produced = fileSize(temporary);
    if (produced >= result.originalBytes) {
        QFile::remove(temporary);
        result.status = CompressResult::Skipped;
        result.note = QStringLiteral("not-smaller");
        report(progress, 100);
        return result;
    }

    QFile::remove(candidate);
    if (!QFile::rename(temporary, candidate)) {
        // 并发下同内容的图片可能已经由另一张写好：尺寸一致就直接复用，避免误判失败
        if (fileSize(candidate) == produced) {
            QFile::remove(temporary);
        } else {
            const QString unique = QDir(workDir).filePath(
                QStringLiteral("%1-q%2-%3.webp")
                    .arg(originalKey)
                    .arg(m_options.imageQuality)
                    .arg(QUuid::createUuid().toString(QUuid::Id128).left(8)));
            if (!QFile::rename(temporary, unique)) {
                QFile::remove(temporary);
                result.status = CompressResult::Failed;
                result.error = QStringLiteral("rename-failed");
                return result;
            }
            result.status = CompressResult::Compressed;
            result.outputPath = unique;
            result.mime = QStringLiteral("image/webp");
            result.method = QStringLiteral("webp-q%1").arg(m_options.imageQuality);
            result.resultBytes = produced;
            result.originalSha256 = hex(sha256File(path));
            result.resultSha256 = hex(sha256File(unique));
            report(progress, 100);
            return result;
        }
    }

    result.status = CompressResult::Compressed;
    result.outputPath = candidate;
    result.mime = QStringLiteral("image/webp");
    result.method = QStringLiteral("webp-q%1").arg(m_options.imageQuality);
    result.resultBytes = produced;
    result.originalSha256 = hex(sha256File(path));
    result.resultSha256 = hex(sha256File(candidate));
    result.note = QStringLiteral("saved %1%")
                      .arg(int((1.0 - double(produced) / double(result.originalBytes)) * 100.0));
    report(progress, 100);
    logger::event(QStringLiteral("compress.image"),
                  QStringLiteral("%1 -> %2 (%3 -> %4)")
                      .arg(QFileInfo(path).fileName())
                      .arg(QFileInfo(candidate).fileName())
                      .arg(result.originalBytes)
                      .arg(produced));
    return result;
}

CompressResult Compressor::prepareCarrierCover(const QString &path, const QString &workDir) const
{
    CompressResult result;
    result.inputPath = path;
    result.outputPath = path;
    result.originalBytes = result.resultBytes = fileSize(path);

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        result.status = CompressResult::Failed;
        result.error = QStringLiteral("decode-failed");
        return result;
    }

    QDir().mkpath(workDir);
    const QString candidate = QDir(workDir).filePath(
        QStringLiteral("cover-%1.jpg").arg(QUuid::createUuid().toString(QUuid::Id128)));
    bool written = false;
    QString writeError;
    {
        QImageWriter writer(candidate, QByteArrayLiteral("jpeg"));
        writer.setQuality(std::clamp(m_options.carrierQuality, 0, 100));
        written = writer.write(image);
        if (!written)
            writeError = writer.errorString();
    }
    const qint64 produced = fileSize(candidate);
    if (!written || produced <= 0 || produced >= result.originalBytes) {
        QFile::remove(candidate);
        result.status = CompressResult::Skipped;
        result.note = written ? QStringLiteral("not-smaller") : writeError;
        return result;
    }

    result.status = CompressResult::Compressed;
    result.outputPath = candidate;
    result.mime = QStringLiteral("image/jpeg");
    result.method = QStringLiteral("jpeg-q%1").arg(m_options.carrierQuality);
    result.resultBytes = produced;
    return result;
}

} // namespace cj
