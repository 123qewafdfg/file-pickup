#include "core/payload.h"

#include "miniz.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <cstring>

namespace cj::bundle {

namespace {

QString baseName(const QString &path)
{
    return QFileInfo(path).fileName();
}

bool isSafeEntryName(const QString &name)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    if (name.contains(QLatin1Char(':')))
        return false;
    return true;
}

} // namespace

bool pack(const QList<BundleItem> &items, const QString &outputPath, QString *error)
{
    if (items.isEmpty()) {
        if (error)
            *error = QStringLiteral("请至少添加一项隐藏内容");
        return false;
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, outputPath.toUtf8().constData(), 0)) {
        if (error)
            *error = QStringLiteral("无法创建打包文件");
        return false;
    }

    QSet<QString> used;
    bool ok = true;
    for (int i = 0; i < items.size(); ++i) {
        const BundleItem &item = items.at(i);
        QString name = baseName(item.path);
        if (name.isEmpty())
            name = QStringLiteral("file-%1").arg(i + 1);
        if (used.contains(name))
            name = QStringLiteral("%1-%2").arg(i + 1).arg(name);
        used.insert(name);

        const QByteArray nameUtf8 = name.toUtf8();
        if (!mz_zip_writer_add_file(&zip,
                                    nameUtf8.constData(),
                                    item.path.toUtf8().constData(),
                                    nullptr,
                                    0,
                                    MZ_DEFAULT_COMPRESSION)) {
            if (error)
                *error = QStringLiteral("打包失败：%1").arg(name);
            ok = false;
            break;
        }
    }

    if (!mz_zip_writer_finalize_archive(&zip))
        ok = false;
    mz_zip_writer_end(&zip);

    if (!ok) {
        QFile::remove(outputPath);
        return false;
    }
    return true;
}

bool unpack(const QString &bundlePath, const QString &outputDir, QList<BundleItem> *items, QString *error)
{
    if (!QFileInfo::exists(bundlePath)) {
        if (error)
            *error = QStringLiteral("恢复压缩包不存在");
        return false;
    }
    if (!QDir().mkpath(outputDir)) {
        if (error)
            *error = QStringLiteral("无法创建解包目录");
        return false;
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, bundlePath.toUtf8().constData(), 0)) {
        if (error)
            *error = QStringLiteral("压缩包无法打开");
        return false;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count == 0) {
        mz_zip_reader_end(&zip);
        if (error)
            *error = QStringLiteral("恢复压缩包为空");
        return false;
    }
    if (count > static_cast<mz_uint>(kMaxEntries)) {
        mz_zip_reader_end(&zip);
        if (error)
            *error = QStringLiteral("压缩包文件数量超过 %1 个").arg(kMaxEntries);
        return false;
    }

    QList<BundleItem> restored;
    QSet<QString> used;
    qint64 total = 0;
    bool ok = true;

    for (mz_uint i = 0; i < count && ok; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            if (error)
                *error = QStringLiteral("压缩包条目损坏");
            ok = false;
            break;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            if (error)
                *error = QStringLiteral("压缩包不能包含文件夹");
            ok = false;
            break;
        }
        const QString name = QString::fromUtf8(stat.m_filename);
        if (!isSafeEntryName(name)) {
            if (error)
                *error = QStringLiteral("压缩包包含不安全路径：%1").arg(name);
            ok = false;
            break;
        }
        if (used.contains(name.toLower())) {
            if (error)
                *error = QStringLiteral("压缩包包含重复文件名：%1").arg(name);
            ok = false;
            break;
        }
        used.insert(name.toLower());

        total += static_cast<qint64>(stat.m_uncomp_size);
        if (total > kMaxExtractedBytes) {
            if (error)
                *error = QStringLiteral("解压后内容超过 700 MB");
            ok = false;
            break;
        }

        const QString target = QDir(outputDir).filePath(name);
        if (!mz_zip_reader_extract_to_file(&zip, i, target.toUtf8().constData(), 0)) {
            if (error)
                *error = QStringLiteral("解压失败：%1").arg(name);
            ok = false;
            break;
        }
        restored.append({name, target, static_cast<qint64>(stat.m_uncomp_size)});
    }

    mz_zip_reader_end(&zip);

    if (!ok) {
        for (const BundleItem &item : std::as_const(restored))
            QFile::remove(item.path);
        return false;
    }
    if (restored.isEmpty()) {
        if (error)
            *error = QStringLiteral("恢复压缩包为空");
        return false;
    }
    if (items)
        *items = restored;
    return true;
}

} // namespace cj::bundle
