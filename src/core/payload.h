#pragma once

#include <QList>
#include <QString>

namespace cj {

struct BundleItem {
    QString name;  // 仅文件名
    QString path;  // 本地绝对路径
    qint64 size = 0;
};

// 多文件打包 / 解包，行为对齐安卓 PayloadBundle：
// 单文件不打包；打包结果固定命名为 hidden-files.zip。
namespace bundle {

constexpr int kMaxEntries = 1000;
constexpr qint64 kMaxExtractedBytes = 700LL * 1024 * 1024;

bool pack(const QList<BundleItem> &items, const QString &outputPath, QString *error);
bool unpack(const QString &bundlePath, const QString &outputDir, QList<BundleItem> *items, QString *error);

} // namespace bundle

} // namespace cj
