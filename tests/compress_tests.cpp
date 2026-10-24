#include "compress_tests.h"

#include "core/compressor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QRandomGenerator>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

static QTextStream *s_out = nullptr;
static int g_passed = 0;
static int g_failed = 0;

namespace {

void expect(bool condition, const QString &label, const QString &detail = QString())
{
    if (condition) {
        ++g_passed;
        *s_out << "[PASS] " << label << "\n";
    } else {
        ++g_failed;
        *s_out << "[FAIL] " << label << (detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail)
               << "\n";
    }
    s_out->flush();
}

QImage noiseImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    QRandomGenerator random(20261022);
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x)
            line[x] = qRgb(random.bounded(256), random.bounded(256), random.bounded(256));
    }
    return image;
}

QString writeImage(const QString &dir, const QString &name, const QImage &image, const char *format,
                   int quality)
{
    const QString path = QDir(dir).filePath(name);
    image.save(path, format, quality);
    return path;
}

void writeRawGif(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    const char bytes[] = {'G', 'I', 'F', '8', '9', 'a', 0x01, 0x00, 0x01, 0, 0, 0, 0, ';'};
    file.write(bytes, sizeof(bytes));
}

} // namespace

int runCompressionTests(QTextStream &out, int *passed)
{
    s_out = &out;
    out << "\n== 压缩 / 校验 ==\n";
    {
        QStringList writable;
        for (const QByteArray &format : QImageWriter::supportedImageFormats())
            writable << QString::fromLatin1(format);
        out << "[INFO] 可写图像格式: " << writable.join(QStringLiteral(", ")) << "\n";
    }

    // 1. 动图识别
    {
        QTemporaryDir dir;
        const QString gif = QDir(dir.path()).filePath(QStringLiteral("a.gif"));
        writeRawGif(gif);
        expect(cj::Compressor::isAnimated(gif), QStringLiteral("GIF 判定为动图"));

        const QString webp = QDir(dir.path()).filePath(QStringLiteral("b.webp"));
        QFile webpFile(webp);
        webpFile.open(QIODevice::WriteOnly);
        QByteArray animated(28, '\0');
        animated.replace(0, 4, "RIFF");
        animated.replace(8, 4, "WEBP");
        animated.replace(12, 4, "VP8X");
        animated[20] = 0x02;
        webpFile.write(animated);
        webpFile.close();
        expect(cj::Compressor::isAnimated(webp), QStringLiteral("动画 WebP 判定为动图"));
    }

    // 2. JPEG 转 WebP：更小、尺寸不变、可解码、SHA-256 记录
    {
        QTemporaryDir dir;
        const QString work = QDir(dir.path()).filePath(QStringLiteral("work"));
        const QString source = writeImage(dir.path(), QStringLiteral("noise.jpg"), noiseImage(900, 600), "JPEG", 100);
        const cj::Compressor compressor{};
        int lastProgress = 0;
        const cj::CompressResult result = compressor.compress(source, work, [&lastProgress](int value) {
            lastProgress = value;
        });
        expect(result.status == cj::CompressResult::Compressed, QStringLiteral("JPEG 压缩被采用"),
               QStringLiteral("status=%1 error=%2 note=%3")
                   .arg(int(result.status))
                   .arg(result.error, result.note));
        if (result.adopted()) {
            expect(result.resultBytes < result.originalBytes, QStringLiteral("压缩后更小"),
                   QStringLiteral("%1 -> %2").arg(result.originalBytes).arg(result.resultBytes));
            QImage decoded(result.outputPath);
            expect(!decoded.isNull() && decoded.size() == QSize(900, 600),
                   QStringLiteral("产物可解码且尺寸不变"));
            expect(result.outputPath.endsWith(QStringLiteral(".webp")), QStringLiteral("产物是 WebP"));
            expect(!result.resultSha256.isEmpty(), QStringLiteral("产物 SHA-256 已记录"));
            expect(lastProgress == 100, QStringLiteral("进度回到 100"), QString::number(lastProgress));
        }
    }

    // 3. PNG 同样走 WebP q75
    {
        QTemporaryDir dir;
        const QString work = QDir(dir.path()).filePath(QStringLiteral("work"));
        const QString source = writeImage(dir.path(), QStringLiteral("noise.png"), noiseImage(400, 300), "PNG", 100);
        const cj::Compressor compressor{};
        const cj::CompressResult result = compressor.compress(source, work);
        expect(result.status == cj::CompressResult::Compressed, QStringLiteral("PNG 压缩被采用"),
               QStringLiteral("error=%1 note=%2").arg(result.error, result.note));
        if (result.adopted())
            expect(QFileInfo(result.outputPath).size() < QFileInfo(source).size(),
                   QStringLiteral("PNG 产物更小"));
    }

    // 4. 开关关闭时不压缩
    {
        QTemporaryDir dir;
        const QString source = writeImage(dir.path(), QStringLiteral("n.png"), noiseImage(200, 200), "PNG", 100);
        cj::CompressOptions options;
        options.images = false;
        const cj::Compressor compressor(options);
        const cj::CompressResult result = compressor.compress(source, QDir(dir.path()).filePath(QStringLiteral("w")));
        expect(result.status == cj::CompressResult::Skipped
                   && result.note == QStringLiteral("images-disabled"),
               QStringLiteral("关闭图片压缩时不处理"), result.note);
    }

    // 5. 非图片不压缩
    {
        QTemporaryDir dir;
        const QString pdf = QDir(dir.path()).filePath(QStringLiteral("doc.pdf"));
        QFile file(pdf);
        file.open(QIODevice::WriteOnly);
        file.write(QByteArray(2048, 'x'));
        file.close();
        const cj::Compressor compressor{};
        const cj::CompressResult result = compressor.compress(pdf, QDir(dir.path()).filePath(QStringLiteral("w")));
        expect(result.status == cj::CompressResult::Skipped
                   && result.note == QStringLiteral("unsupported-type"),
               QStringLiteral("非图片类型跳过"), result.note);
    }

    out << "== 压缩自测：" << g_passed << " 通过 / " << g_failed << " 失败 ==\n";
    if (passed != nullptr)
        *passed += g_passed;
    s_out = nullptr;
    return g_failed;
}
