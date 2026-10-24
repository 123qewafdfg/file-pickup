#include "app/appcontroller.h"

#include "core/compressor.h"
#include "core/crypto.h"
#include "core/logger.h"
#include "core/partition.h"
#include "core/payload.h"
#include "core/securestore.h"
#include "core/tmshare.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QImageReader>
#include <QSemaphore>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

#include <algorithm>
#include <functional>
#include <thread>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

// 压缩并发内存预算：单份 24 MiB，总共 24 份 ≈ 576 MiB 在飞
constexpr qint64 kUnitBytes = 24LL * 1024 * 1024;
constexpr int kBudgetUnits = 16;   // 可用 CJ_COMPRESS_UNITS 覆盖（1 份 ≈ 24 MiB）

} // namespace

namespace cj {

namespace {

QString kindOf(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QStringList images = {QStringLiteral("jpg"),  QStringLiteral("jpeg"),
                                       QStringLiteral("png"),  QStringLiteral("webp"),
                                       QStringLiteral("gif"),  QStringLiteral("bmp"),
                                       QStringLiteral("heic"), QStringLiteral("heif")};
    static const QStringList videos = {QStringLiteral("mp4"), QStringLiteral("mov"),
                                       QStringLiteral("mkv"), QStringLiteral("webm"),
                                       QStringLiteral("avi"), QStringLiteral("m4v")};
    if (images.contains(suffix))
        return QStringLiteral("图片");
    if (videos.contains(suffix))
        return QStringLiteral("视频");
    return QStringLiteral("文件");
}

QString mimeOf(const QString &path)
{
    return QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name();
}

QString formatSize(qint64 bytes)
{
    if (bytes < 1000)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1000LL * 1000)
        return QStringLiteral("%1 KB").arg(bytes / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / 1000000.0, 0, 'f', 1);
}

QProcessEnvironment directUploadEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    static const QStringList proxyKeys{
        QStringLiteral("HTTP_PROXY"), QStringLiteral("HTTPS_PROXY"),
        QStringLiteral("ALL_PROXY"), QStringLiteral("http_proxy"),
        QStringLiteral("https_proxy"), QStringLiteral("all_proxy")};
    for (const QString &key : proxyKeys)
        environment.remove(key);
    environment.insert(QStringLiteral("NO_PROXY"), QStringLiteral("*"));
    environment.insert(QStringLiteral("no_proxy"), QStringLiteral("*"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    return environment;
}

QString embeddedScriptPath(const QString &fileName)
{
    static const QStringList scriptNames{
        QStringLiteral("qq_channel.py"),
        QStringLiteral("bili_dynamic.py"),
        QStringLiteral("feejii.py"),
        QStringLiteral("join_ui.py"),
        QStringLiteral("cdp_capture.py")};
    if (!scriptNames.contains(fileName))
        return {};

    const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                             .filePath(QStringLiteral("runtime/python-scripts"));
    if (!QDir().mkpath(root)) {
        logger::event(QStringLiteral("python.script-dir-fail"), root);
        return {};
    }

    // 每次检查资源内容；只有版本变化时才原子替换磁盘缓存。
    for (const QString &name : scriptNames) {
        QFile resource(QStringLiteral(":/python/") + name);
        if (!resource.open(QIODevice::ReadOnly)) {
            logger::event(QStringLiteral("python.resource-missing"), name);
            return {};
        }
        const QByteArray expected = resource.readAll();
        const QString outputPath = QDir(root).filePath(name);

        QFile current(outputPath);
        if (current.open(QIODevice::ReadOnly)
            && QCryptographicHash::hash(current.readAll(), QCryptographicHash::Sha256)
                   == QCryptographicHash::hash(expected, QCryptographicHash::Sha256)) {
            continue;
        }

        QSaveFile output(outputPath);
        if (!output.open(QIODevice::WriteOnly) || output.write(expected) != expected.size()
            || !output.commit()) {
            logger::event(QStringLiteral("python.script-write-fail"), outputPath);
            return {};
        }
    }
    return QDir(root).filePath(fileName);
}

QStringList pythonScriptArguments(const QString &script, const QStringList &arguments = {})
{
    // Embeddable Python 的 ._pth 隔离模式不会自动把脚本目录加入 sys.path。
    // 通过 runpy 显式加入后再执行，既支持同目录模块，又不污染全局 Python 环境。
    static const QString bootstrap = QStringLiteral(
        "import os,runpy,sys;"
        "p=sys.argv.pop(1);"
        "sys.path.insert(0,os.path.dirname(p));"
        "runpy.run_path(p,run_name='__main__')");
    QStringList result{QStringLiteral("-X"), QStringLiteral("utf8"), QStringLiteral("-c"),
                       bootstrap, script};
    result.append(arguments);
    return result;
}

struct MakeResult {
    bool ok = false;
    QString error;
    CreatedResource resource;
    QqCarrierPlan qqPlan;
    bool qqStreaming = false;
    bool feijiiStreaming = false;
    QString outputDir;
};

struct QqPartResult {
    bool ok = false;
    QString error;
    CarrierPart part;
    QByteArray streamState;
};

} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_settings()
    , m_backend(new Backend(this))
{
    m_qqLogin = new QqQrLogin(this);
    m_channel = new ChannelApi(this);
    connect(m_qqLogin, &QqQrLogin::loginSucceeded, this, [this]() {
        m_channel->setSessionCookies(m_qqLogin->sessionCookies());
        refreshAccounts();
        emit notify(QStringLiteral("QQ 频道已登录"));
        QTimer::singleShot(0, this, [this]() { startAutoJoin(); });
    });

    const QString probeMb = qEnvironmentVariable("CJ_UPLOAD_TEST");
    if (!probeMb.isEmpty()) {
        QTimer::singleShot(1200, this, [this, probeMb]() {
            m_qqLogin->restoreSession();
            m_channel->setSessionCookies(m_qqLogin->sessionCookies());
            logger::reset();
            runUploadProbe(probeMb.toInt());
        });
    }

    loadSettings();
    loadHistory();
    // 上次中断留下的下载分片/临时恢复目录，启动时清掉，省盘
    QTimer::singleShot(0, this, [this]() { pruneCache(); });
    // 上次登录的会话（DPAPI 加密保存）在启动时恢复，避免每次重新扫码
    if (m_qqLogin->restoreSession()) {
        m_channel->setSessionCookies(m_qqLogin->sessionCookies());
    }
    refreshAccounts();
}

void AppController::loadSettings()
{
    // 默认跟随系统主题；用户手动切换后以保存值为准
    const bool systemDark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    m_dark = m_settings.value(QStringLiteral("dark"), systemDark).toBool();
    m_compressImages = m_settings.value(QStringLiteral("compressImages"), true).toBool();
    m_fullCheck = m_settings.value(QStringLiteral("fullCheck"), true).toBool();
    m_platform = m_settings.value(QStringLiteral("platform"), QStringLiteral("小飞机网盘")).toString();
    const QString feijiiDataRoot = qEnvironmentVariable("LOCALAPPDATA").isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        : qEnvironmentVariable("LOCALAPPDATA");
    const QString feijiiSession = QDir(feijiiDataRoot)
        .filePath(QStringLiteral("FilePickup/feejii-session.dat"));
    m_feijiiConnected = QFileInfo::exists(feijiiSession);
    m_settings.setValue(QStringLiteral("feijiiConnected"), m_feijiiConnected);
    // 发布是显式选择：没有保存过设置时默认只上传，绝不自动发帖/动态。
    m_publishToChannel = m_settings.value(QStringLiteral("publishToChannel"), false).toBool();
    m_windowWidth = m_settings.value(QStringLiteral("windowWidth"), 1100).toInt();
    m_windowHeight = m_settings.value(QStringLiteral("windowHeight"), 700).toInt();
    m_outputDir = m_settings.value(QStringLiteral("outputDir")).toString();
    if (m_outputDir.isEmpty()) {
        m_outputDir = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                          .filePath(QStringLiteral("藏笺载体"));
    }
    m_extractDir = m_settings.value(QStringLiteral("extractDir")).toString();
    if (m_extractDir.isEmpty()) {
        m_extractDir = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                           .filePath(QStringLiteral("藏笺提取"));
    }
    m_extractSessionDir = m_extractDir;
    const QJsonDocument indexDoc =
        QJsonDocument::fromJson(m_settings.value(QStringLiteral("extractIndex")).toString().toUtf8());
    if (indexDoc.isObject())
        m_extractIndex = indexDoc.object();
}

void AppController::setDarkMode(bool value)
{
    if (m_dark == value)
        return;
    m_dark = value;
    m_settings.setValue(QStringLiteral("dark"), value);
    emit settingsChanged();
}

void AppController::setCompressImages(bool value)
{
    if (m_compressImages == value)
        return;
    m_compressImages = value;
    m_settings.setValue(QStringLiteral("compressImages"), value);
    emit settingsChanged();
    // 打开开关后，把已在列表里但还没转换的图片补转
    if (value)
        compressPendingFiles();
}

void AppController::setFullCheck(bool value)
{
    if (m_fullCheck == value)
        return;
    m_fullCheck = value;
    m_settings.setValue(QStringLiteral("fullCheck"), value);
    emit settingsChanged();
}

void AppController::setPublishToChannel(bool value)
{
    if (m_publishToChannel == value)
        return;
    m_publishToChannel = value;
    m_settings.setValue(QStringLiteral("publishToChannel"), value);
    emit settingsChanged();
}

void AppController::setPlatform(const QString &value)
{
    if (m_platform == value || value.isEmpty())
        return;
    m_platform = value;
    m_settings.setValue(QStringLiteral("platform"), value);
    emit settingsChanged();
}

void AppController::setCode(const QString &value)
{
    QString normalized;
    normalized.reserve(5);
    for (const QChar ch : value) {
        const ushort unicode = ch.unicode();
        if ((unicode >= 'A' && unicode <= 'Z') || (unicode >= 'a' && unicode <= 'z'))
            normalized.append(ch.toUpper());
        if (normalized.size() == 5)
            break;
    }
    if (m_code == normalized) {
        if (value != normalized)
            emit codeChanged();
        return;
    }
    m_code = normalized;
    emit codeChanged();
}

void AppController::setBusy(bool value, const QString &stage)
{
    m_busy = value;
    m_stage = stage;
    emit busyChanged();
    emit stageChanged();
}

QString AppController::cacheDir()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(root);
    return root;
}

QString AppController::makeWorkDir()
{
    const QString path = QDir(cacheDir()).filePath(QStringLiteral("generated"));
    QDir().mkpath(path);
    return path;
}

void AppController::setExtractDir(const QString &value)
{
    QString path = value.trimmed();
    if (path.startsWith(QStringLiteral("file://")))
        path = QUrl(path).toLocalFile();
    if (path.isEmpty())
        return;
    path = QDir::cleanPath(QDir(path).absolutePath());
    if (path == m_extractDir)
        return;
    m_extractDir = path;
    m_extractSessionDir = path;
    m_settings.setValue(QStringLiteral("extractDir"), path);
    emit extractDirChanged();
    emit extractSessionDirChanged();
    emit notify(QStringLiteral("提取目录已改为：%1").arg(path));
}

void AppController::setForceExtract(bool value)
{
    if (m_forceExtract == value)
        return;
    m_forceExtract = value;
    emit settingsChanged();
}

// 记下这次提取的口令指纹，下次同口令先比对内容是否一样
void AppController::rememberExtraction(const QString &code, const QString &sha, qint64 bytes)
{
    if (code.isEmpty() || sha.isEmpty())
        return;
    QJsonObject entry;
    entry.insert(QStringLiteral("sha"), sha);
    entry.insert(QStringLiteral("size"), static_cast<double>(bytes));
    entry.insert(QStringLiteral("time"),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    entry.insert(QStringLiteral("dir"), m_extractSessionDir);
    m_extractIndex.insert(code, entry);
    m_settings.setValue(QStringLiteral("extractIndex"),
                        QString::fromUtf8(QJsonDocument(m_extractIndex).toJson(QJsonDocument::Compact)));
}

void AppController::dismissDuplicatePrompt()
{
    if (!m_duplicatePrompt)
        return;
    m_duplicatePrompt = false;
    emit duplicatePromptChanged();
}

// 弹窗里点「继续提取」：强制重来一次
void AppController::continueAfterDuplicate()
{
    m_duplicatePrompt = false;
    m_duplicateDir.clear();
    emit duplicatePromptChanged();
    m_forceOnce = true;
    startExtract();
}

void AppController::openDuplicateDir()
{
    const QString dir = m_duplicateDir;
    dismissDuplicatePrompt();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        emit notify(QStringLiteral("之前的提取文件夹已经不在了"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void AppController::openExtractDir()
{
    const QString target = QDir(m_extractSessionDir).exists() ? m_extractSessionDir : m_extractDir;
    QDir().mkpath(target);
    QDesktopServices::openUrl(QUrl::fromLocalFile(target));
}

void AppController::clearExtractDir()
{
    QDir root(m_extractDir);
    if (!root.exists()) {
        emit notify(QStringLiteral("目录还不存在"));
        return;
    }
    qint64 freed = 0;
    int removed = 0;
    const QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : entries) {
        if (info.isDir()) {
            QDir dir(info.absoluteFilePath());
            const auto files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
            for (const QFileInfo &file : files)
                freed += file.size();
            if (dir.removeRecursively())
                ++removed;
        } else {
            freed += info.size();
            if (QFile::remove(info.absoluteFilePath()))
                ++removed;
        }
    }
    logger::event(QStringLiteral("extract.clear"),
                  QStringLiteral("removed=%1 freed=%2").arg(removed).arg(freed));
    emit notify(QStringLiteral("已清理 %1 项，释放 %2").arg(removed).arg(formatSize(freed)));
}

// 每次提取单独一个时间戳目录，避免上一次的结果被覆盖
QString AppController::newExtractSessionDir()
{
    QDir root(m_extractDir);
    QDir().mkpath(root.absolutePath());
    const QString base = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString path = root.filePath(base);
    int suffix = 2;
    while (QDir(path).exists())
        path = root.filePath(QStringLiteral("%1-%2").arg(base).arg(suffix++));
    QDir().mkpath(path);
    m_extractSessionDir = path;
    emit extractSessionDirChanged();
    return path;
}

// 清理缓存里的下载残留与上次的临时恢复目录
void AppController::pruneCache()
{
    const QDir cache(cacheDir());
    qint64 freed = 0;
    const QFileInfoList entries = cache.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : entries) {
        const QString name = info.fileName();
        if (!name.startsWith(QStringLiteral("download-"))
            && name != QStringLiteral("restored")
            && name != QStringLiteral("probe")) {
            continue;
        }
        QDir dir(info.absoluteFilePath());
        const auto files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &file : files)
            freed += file.size();
        dir.removeRecursively();
    }
    if (freed > 0) {
        logger::event(QStringLiteral("cache.prune"),
                      QStringLiteral("freed=%1 B").arg(freed));
    }
}

void AppController::addUrls(const QStringList &urls)
{
    int added = 0;
    for (const QString &raw : urls) {
        QString path = raw;
        if (path.startsWith(QStringLiteral("file://"))) {
            const QUrl url(path);
            path = url.toLocalFile();
        }
        QFileInfo info(path);
        if (!info.exists() || !info.isFile())
            continue;

        QVariantMap item;
        item.insert(QStringLiteral("name"), info.fileName());
        item.insert(QStringLiteral("path"), info.absoluteFilePath());
        item.insert(QStringLiteral("sourcePath"), info.absoluteFilePath());
        item.insert(QStringLiteral("kind"), kindOf(path));
        item.insert(QStringLiteral("size"), info.size());
        item.insert(QStringLiteral("meta"),
                    QStringLiteral("%1 · %2").arg(kindOf(path), formatSize(info.size())));
        m_files.append(item);
        ++added;
    }
    if (added == 0) {
        emit notify(QStringLiteral("没有可用的文件"));
        return;
    }
    emit filesChanged();
    emit notify(QStringLiteral("已添加 %1 项内容").arg(added));
    compressPendingFiles();
}

void AppController::addText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        emit notify(QStringLiteral("请先输入文字"));
        return;
    }
    const QByteArray utf8 = trimmed.toUtf8();
    if (utf8.size() > 1000) {
        emit notify(QStringLiteral("文字内容不能超过 1000 字节（当前 %1）").arg(utf8.size()));
        return;
    }
    const QString dir = QDir(cacheDir()).filePath(QStringLiteral("text"));
    QDir().mkpath(dir);
    const QString path =
        QDir(dir).filePath(QStringLiteral("text-%1.txt").arg(QDateTime::currentMSecsSinceEpoch()));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        emit notify(QStringLiteral("无法保存文字内容"));
        return;
    }
    file.write(utf8);
    file.close();

    QString name = trimmed.section(QLatin1Char('\n'), 0, 0).left(40);
    if (name.isEmpty())
        name = QStringLiteral("文字.txt");

    QVariantMap item;
    item.insert(QStringLiteral("name"), name);
    item.insert(QStringLiteral("path"), path);
    item.insert(QStringLiteral("kind"), QStringLiteral("文字"));
    item.insert(QStringLiteral("size"), static_cast<qint64>(utf8.size()));
    item.insert(QStringLiteral("meta"),
                QStringLiteral("文字 · %1 字符").arg(QString::fromUtf8(utf8).size()));
    m_files.append(item);
    emit filesChanged();
}

void AppController::removeAt(int index)
{
    if (index < 0 || index >= m_files.size())
        return;
    m_files.removeAt(index);
    emit filesChanged();
}

void AppController::clearFiles()
{
    if (m_files.isEmpty())
        return;
    m_files.clear();
    emit filesChanged();
    if (!m_compressInfo.isEmpty()) {
        m_compressInfo.clear();
        emit compressInfoChanged();
    }
}

QString AppController::readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.read(200000));
}

QString AppController::humanSize(qint64 bytes) const
{
    return formatSize(bytes);
}

void AppController::openPath(const QString &path)
{
    if (path.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void AppController::revealPath(const QString &path)
{
    if (path.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void AppController::resetMake()
{
    m_files.clear();
    m_carriers.clear();
    m_code.clear();
    emit codeChanged();
    m_created = {};
    emit filesChanged();
    emit carriersChanged();
}

void AppController::openOutputDir()
{
    if (m_outputDir.isEmpty())
        return;
    QDir().mkpath(m_outputDir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_outputDir));
}

namespace {

// 压缩模块返回的 note 是机器码，界面要给用户看中文
QString compressNoteText(const QString &note)
{
    static const QHash<QString, QString> texts{
        {QStringLiteral("not-smaller"), QStringLiteral("已是最优，不压缩")},
        {QStringLiteral("animated"), QStringLiteral("动图不压缩")},
        {QStringLiteral("images-disabled"), QStringLiteral("图片压缩已关闭")},
        {QStringLiteral("unsupported-type"), QStringLiteral("该类型不压缩（只压图片）")},
    };
    return texts.value(note, note);
}

} // namespace

void AppController::scheduleFilesChanged()
{
    if (m_filesFlushScheduled)
        return;
    m_filesFlushScheduled = true;
    QTimer::singleShot(80, this, [this]() {
        m_filesFlushScheduled = false;
        emit filesChanged();
    });
}

void AppController::compressPendingFiles()
{
    if (m_compressBusy || !m_compressImages)
        return;

    struct Pending {
        int index;
        QString path;
        QString displayName;
        int units;   // 解码后的内存权重，用于限制同时在飞的图片总内存
    };
    QVector<Pending> pending;
    for (int i = 0; i < m_files.size(); ++i) {
        const QVariantMap item = m_files.at(i).toMap();
        if (item.value(QStringLiteral("converted")).toBool())
            continue;
        if (item.value(QStringLiteral("kind")).toString() != QStringLiteral("图片"))
            continue;
        const QString path = item.value(QStringLiteral("path")).toString();
        if (!cj::Compressor::isImage(path) || cj::Compressor::isAnimated(path))
            continue;
        // 按解码后的像素内存估权重（32bit/像素），大图独占更多配额
        QSize pixelSize;
        {
            QImageReader probe(path);
            pixelSize = probe.size();
        }
        const qint64 bytes = pixelSize.isValid()
                                 ? qint64(pixelSize.width()) * pixelSize.height() * 4
                                 : 0;
        const int units = std::clamp(int((bytes + kUnitBytes - 1) / kUnitBytes), 1, kBudgetUnits);
        pending.append({i, path, item.value(QStringLiteral("name")).toString(), units});
    }
    if (pending.isEmpty())
        return;

    m_compressBusy = true;
    m_compressProgress = 0;
    m_compressDone = 0;
    m_compressConverted = 0;
    m_compressBefore = 0;
    m_compressAfter = 0;
    m_compressNotes.clear();
    m_stage = QStringLiteral("正在转换图片格式…");
    emit stageChanged();
    emit compressProgressChanged();
    emit notify(QStringLiteral("正在转换 %1 张图片…").arg(pending.size()));

    const QString workDir = QDir(cacheDir()).filePath(QStringLiteral("compress"));
    const int quality = 75;
    const int total = int(pending.size());
    // 注意：环境变量未设置时 qEnvironmentVariableIntValue 返回 0，不能让 0 变成配额，
    // 否则小配额 + 大图权重会永久阻塞在 acquire 上（曾导致界面卡死）
    const int envUnits = qEnvironmentVariableIntValue("CJ_COMPRESS_UNITS");
    const int budgetUnits = envUnits > 0 ? std::clamp(envUnits, 2, 64) : kBudgetUnits;
    auto budget = QSharedPointer<QSemaphore>::create(budgetUnits);

    // 多线程：每张图独立编码，线程数按 CPU 核数放大（Qt WebP 编码是单线程的，只能靠并行抬速度）
    // 解码后单张图常占几十 MB，线程过多会把内存推满导致整机变卡；取核数一半并限制在 4~8
    const int cores = int(std::thread::hardware_concurrency());
    const int threads = std::clamp(cores / 2, 4, 8);
    QThreadPool *pool = QThreadPool::globalInstance();
    if (pool->maxThreadCount() < threads)
        pool->setMaxThreadCount(threads);

    auto *watcher = new QFutureWatcher<QVariantMap>(this);

    // 增量回写：每完成一张就更新列表与进度，不等全部结束
    connect(watcher, &QFutureWatcher<QVariantMap>::resultReadyAt, this,
            [this, watcher, pending, total](int index) {
                if (index < 0 || index >= pending.size())
                    return;
                const QVariantMap result = watcher->resultAt(index);
                const int fileIndex = pending.at(index).index;
                if (fileIndex < 0 || fileIndex >= m_files.size())
                    return;

                QVariantMap item = m_files.at(fileIndex).toMap();
                const qint64 originalBytes = result.value(QStringLiteral("originalBytes")).toLongLong();
                if (result.value(QStringLiteral("ok")).toBool()) {
                    const QString path = result.value(QStringLiteral("path")).toString();
                    const qint64 bytes = result.value(QStringLiteral("bytes")).toLongLong();
                    const QFileInfo original(item.value(QStringLiteral("name")).toString());
                    item.insert(QStringLiteral("path"), path);
                    item.insert(QStringLiteral("name"),
                                original.completeBaseName() + QLatin1Char('.')
                                    + QFileInfo(path).suffix());
                    item.insert(QStringLiteral("size"), bytes);
                    item.insert(QStringLiteral("meta"),
                                QStringLiteral("图片 · %1（原 %2）")
                                    .arg(formatSize(bytes), formatSize(originalBytes)));
                    m_compressBefore += originalBytes;
                    m_compressAfter += bytes;
                    ++m_compressConverted;
                } else {
                    const QString note = result.value(QStringLiteral("note")).toString();
                    const QString text = compressNoteText(note);
                    if (!text.isEmpty() && !m_compressNotes.contains(text))
                        m_compressNotes << text;
                }
                if (!item.contains(QStringLiteral("originalSize")))
                    item.insert(QStringLiteral("originalSize"), originalBytes);
                item.insert(QStringLiteral("converted"), true);
                m_files[fileIndex] = item;
                scheduleFilesChanged();

                ++m_compressDone;
                m_compressProgress = total > 0 ? int(100.0 * double(m_compressDone) / double(total)) : 100;
                emit compressProgressChanged();
                logger::event(QStringLiteral("compress.progress"),
                              QStringLiteral("%1/%2").arg(m_compressDone).arg(total));
            });

    connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [this, watcher, total, quality]() {
        watcher->deleteLater();
        m_compressBusy = false;
        m_compressProgress = 100;
        emit filesChanged();
        emit compressProgressChanged();

        QString summary;
        if (m_compressConverted > 0) {
            summary = QStringLiteral("已转 WebP q%1 · %2 → %3（省 %4%）")
                          .arg(quality)
                          .arg(formatSize(m_compressBefore), formatSize(m_compressAfter))
                          .arg(m_compressBefore > 0
                                   ? int((1.0 - double(m_compressAfter) / double(m_compressBefore)) * 100.0)
                                   : 0);
        }
        const int skipped = total - m_compressConverted;
        if (skipped > 0 && !m_compressNotes.isEmpty())
            summary += (summary.isEmpty() ? QString() : QStringLiteral("；"))
                + QStringLiteral("%1 张未转换（%2）").arg(skipped).arg(m_compressNotes.join(QStringLiteral("、")));
        m_compressInfo = summary;
        emit compressInfoChanged();
        m_stage.clear();
        emit stageChanged();
        if (m_compressConverted > 0)
            emit notify(QStringLiteral("已转换 %1 张图片为 WebP").arg(m_compressConverted));

        if (m_makeAfterCompress) {
            m_makeAfterCompress = false;
            QTimer::singleShot(0, this, [this]() { startMake(); });
        }
    });

    watcher->setFuture(QtConcurrent::mapped(pending, [workDir, quality, budget, budgetUnits](const Pending &entry) {
        // 内存闸门：大图会占多份配额，保证同时在解码的图片总内存不超过预算
        const int need = std::min(entry.units, budgetUnits);
        budget->acquire(need);
        const QSemaphoreReleaser releaser(*budget, need);
        cj::CompressOptions options;
        options.images = true;
        options.imageQuality = quality;
        const cj::Compressor compressor(options);
        const cj::CompressResult result = compressor.compress(entry.path, workDir);
        QVariantMap map;
        map.insert(QStringLiteral("index"), entry.index);
        map.insert(QStringLiteral("originalBytes"), result.originalBytes);
        if (result.adopted()) {
            map.insert(QStringLiteral("ok"), true);
            map.insert(QStringLiteral("path"), result.outputPath);
            map.insert(QStringLiteral("bytes"), result.resultBytes);
        } else {
            map.insert(QStringLiteral("ok"), false);
            map.insert(QStringLiteral("note"), result.note.isEmpty() ? result.error : result.note);
        }
        return map;
    }));
}

void AppController::startMake()
{
    if (m_busy)
        return;
    if (m_files.isEmpty()) {
        emit notify(QStringLiteral("请先选择内容"));
        return;
    }
    if (m_compressBusy) {
        // 转换还没结束就点了制作：排队，转换完成后自动开始
        m_makeAfterCompress = true;
        emit notify(QStringLiteral("正在转换图片格式，完成后自动开始制作"));
        return;
    }

    // 新一轮发放：清掉上一轮的口令与结果
    if (!m_code.isEmpty()) {
        m_code.clear();
        emit codeChanged();
    }
    m_outputs.clear();
    emit outputsChanged();
    resetProgress({});

    const bool qqMode = m_platform == QStringLiteral("QQ 频道");
    const bool feijiiMode = m_platform == QStringLiteral("小飞机网盘");
    const bool streamMode = qqMode || feijiiMode;
    const QString workDir = makeWorkDir();
    // 不同 Debug 包或多个实例共享 CacheLocation。目录必须跨进程唯一，避免一个任务
    // 完成清理时删掉另一个任务刚生成、尚未上传的流式分片。
    const QString stamp = QStringLiteral("%1-%2-p%3")
                              .arg(QDateTime::currentDateTime().toString(
                                       QStringLiteral("yyyyMMdd-HHmmss")))
                              .arg(QDateTime::currentMSecsSinceEpoch() % 1000, 3, 10, QLatin1Char('0'))
                              .arg(QCoreApplication::applicationPid());
    const QString targetDir = QDir(m_outputDir).filePath(stamp);

    // 多文件打包；单文件先试“智能压缩”，压不动就原样塞进去
    QString payloadPath;
    QString displayName;
    QString mime;
    bool payloadIsBundle = false;
    if (m_files.size() == 1) {
        const QVariantMap item = m_files.first().toMap();
        payloadPath = item.value(QStringLiteral("path")).toString();
        displayName = item.value(QStringLiteral("name")).toString();
        const bool isText = item.value(QStringLiteral("kind")).toString() == QStringLiteral("文字");
        mime = isText ? QStringLiteral("text/plain") : mimeOf(payloadPath);

        // 已经是压缩格式的（图片、视频、zip、office 容器）再 deflate 只会更大，跳过
        static const QSet<QString> kAlreadyPacked{
            QStringLiteral("zip"),  QStringLiteral("7z"),   QStringLiteral("rar"),
            QStringLiteral("gz"),   QStringLiteral("bz2"),  QStringLiteral("xz"),
            QStringLiteral("zst"),  QStringLiteral("apk"),  QStringLiteral("jar"),
            QStringLiteral("docx"), QStringLiteral("xlsx"), QStringLiteral("pptx"),
            QStringLiteral("webp"), QStringLiteral("jpg"),  QStringLiteral("jpeg"),
            QStringLiteral("png"),  QStringLiteral("gif"),  QStringLiteral("avif"),
            QStringLiteral("heic"), QStringLiteral("mp4"),  QStringLiteral("mkv"),
            QStringLiteral("mov"),  QStringLiteral("mp3"),  QStringLiteral("flac"),
            QStringLiteral("ogg")};

        const qint64 originalBytes = QFileInfo(payloadPath).size();
        const QString suffix = QFileInfo(payloadPath).suffix().toLower();
        if (!isText && originalBytes > 64 * 1024 && !kAlreadyPacked.contains(suffix)) {
            QList<BundleItem> one;
            one.append({displayName, payloadPath, originalBytes});
            const QString zipPath = QDir(cacheDir()).filePath(
                QStringLiteral("smart-%1.zip").arg(QDateTime::currentMSecsSinceEpoch()));
            QString packError;
            if (bundle::pack(one, zipPath, &packError)) {
                const qint64 zipBytes = QFileInfo(zipPath).size();
                if (zipBytes > 0 && zipBytes * 100 < originalBytes * 98) {
                    logger::event(QStringLiteral("make.smart-compress"),
                                  QStringLiteral("%1 B -> %2 B").arg(originalBytes).arg(zipBytes));
                    emit notify(QStringLiteral("已智能压缩：%1 → %2")
                                    .arg(formatSize(originalBytes), formatSize(zipBytes)));
                    payloadPath = zipPath;
                    mime = QStringLiteral("application/zip");
                    payloadIsBundle = true;
                } else {
                    QFile::remove(zipPath);
                    logger::event(QStringLiteral("make.smart-compress-skip"),
                                  QStringLiteral("%1 B 压不动，保持原样").arg(originalBytes));
                }
            }
        }
    } else {
        QList<BundleItem> items;
        for (const QVariant &entry : std::as_const(m_files)) {
            const QVariantMap map = entry.toMap();
            items.append({map.value(QStringLiteral("name")).toString(),
                          map.value(QStringLiteral("path")).toString(),
                          map.value(QStringLiteral("size")).toLongLong()});
        }
        const QString bundlePath = QDir(cacheDir()).filePath(
            QStringLiteral("hidden-files-%1.zip").arg(QDateTime::currentMSecsSinceEpoch()));
        QString packError;
        if (!bundle::pack(items, bundlePath, &packError)) {
            emit notify(packError.isEmpty() ? QStringLiteral("打包失败") : packError);
            return;
        }
        payloadPath = bundlePath;
        displayName = QStringLiteral("hidden-files.zip");
        mime = QStringLiteral("application/zip");
        payloadIsBundle = true;
    }

    logger::reset();
    logger::event(QStringLiteral("make.start"),
                  QStringLiteral("files=%1 platform=%2 qqMode=%3")
                      .arg(m_files.size())
                      .arg(m_platform)
                      .arg(qqMode ? 1 : 0));
    logger::event(QStringLiteral("make.payload"), QStringLiteral("%1 (%2 B)").arg(displayName).arg(QFileInfo(payloadPath).size()));

    // B站必须用固定预设承载图：小图/纯色图会被 CDN 重编码，追加的数据会丢
    QByteArray cover = qqMode ? whiteQqCarrierPng()
                              : feijiiMode ? QByteArray() : biliCarrierPng();
    if (qqMode) {
        for (const QVariant &entry : std::as_const(m_files)) {
            const QVariantMap item = entry.toMap();
            if (item.value(QStringLiteral("kind")).toString() != QStringLiteral("图片"))
                continue;
            const QString sourcePath = item.value(QStringLiteral("sourcePath"),
                                                  item.value(QStringLiteral("path"))).toString();
            const Compressor compressor;
            const CompressResult prepared = compressor.prepareCarrierCover(
                sourcePath, QDir(cacheDir()).filePath(QStringLiteral("covers")));
            if (prepared.adopted()) {
                QFile file(prepared.outputPath);
                if (file.open(QIODevice::ReadOnly))
                    cover = file.readAll();
                file.close();
                QFile::remove(prepared.outputPath);
                if (cleanCarrierImageLength(cover) == cover.size()) {
                    logger::event(QStringLiteral("make.cover"),
                                  QStringLiteral("jpeg-q95 bytes=%1").arg(cover.size()));
                } else {
                    cover = whiteQqCarrierPng();
                }
            }
            break;
        }
    }
    if (cover.isEmpty() && !feijiiMode) {
        if (qqMode) {
            cover = whiteQqCarrierPng();
        } else {
            setBusy(false, {});
            logger::event(QStringLiteral("make.carrier-missing"));
            emit notify(QStringLiteral("缺少 B站承载图资源，无法生成载体"));
            return;
        }
    }
    setBusy(true, QStringLiteral("正在生成载体…"));

    auto *watcher = new QFutureWatcher<MakeResult>(this);
    connect(watcher, &QFutureWatcher<MakeResult>::finished, this,
            [this, watcher, payloadPath, qqMode, feijiiMode]() {
        const MakeResult result = watcher->result();
        watcher->deleteLater();
        setBusy(false, {});
        if (!result.ok) {
            if (payloadPath.startsWith(cacheDir()))
                QFile::remove(payloadPath);
            emit notify(result.error.isEmpty() ? QStringLiteral("生成失败") : result.error);
            return;
        }

        if (result.qqStreaming || result.feijiiStreaming) {
            m_created = result.qqPlan.resource;
            m_carriers.clear();
            emit carriersChanged();
            logger::event(feijiiMode ? QStringLiteral("make.feijii-stream-ready")
                                     : QStringLiteral("make.qq-stream-ready"),
                          QStringLiteral("parts=%1 bytes=%2")
                              .arg(result.qqPlan.plainSizes.size())
                              .arg(result.qqPlan.resource.originalSize));
            emit notify(QStringLiteral("流式加密计划已准备，共 %1 个分片，开始逐片上传")
                            .arg(result.qqPlan.plainSizes.size()));
            if (feijiiMode && feijiiConnected()) {
                startFeijiiStream(result.qqPlan, payloadPath);
            } else if (qqMode && m_qqLogin && m_qqLogin->loggedIn()) {
                startQqStream(result.qqPlan, payloadPath);
            } else {
                if (payloadPath.startsWith(cacheDir()))
                    QFile::remove(payloadPath);
                logger::event(QStringLiteral("make.need-login"), m_platform);
                emit notify(feijiiMode ? QStringLiteral("请先登录小飞机网盘")
                                       : QStringLiteral("请先登录 QQ 频道"));
                emit requestRoute(QStringLiteral("login"));
            }
            return;
        }

        // 非流式平台到这里已经生成完全部载体；缓存载荷可以删除。
        if (payloadPath.startsWith(cacheDir()))
            QFile::remove(payloadPath);
        m_created = result.resource;
        m_carriers.clear();
        for (const CarrierPart &part : result.resource.parts) {
            QVariantMap map;
            map.insert(QStringLiteral("name"), QFileInfo(part.path).fileName());
            map.insert(QStringLiteral("path"), part.path);
            map.insert(QStringLiteral("sha"), part.remoteSha256Hex);
            map.insert(QStringLiteral("size"), QFileInfo(part.path).size());
            map.insert(QStringLiteral("meta"), formatSize(QFileInfo(part.path).size()));
            m_carriers.append(map);
        }
        m_outputDir = result.outputDir;
        m_settings.setValue(QStringLiteral("outputDir"), m_outputDir);
        emit outputDirChanged();
        emit carriersChanged();
        logger::event(QStringLiteral("make.carriers"),
                      QStringLiteral("count=%1 bytes=%2")
                          .arg(m_carriers.size())
                          .arg([this]() {
                              qint64 sum = 0;
                              for (const QVariant &entry : std::as_const(m_carriers))
                                  sum += entry.toMap().value(QStringLiteral("size")).toLongLong();
                              return sum;
                          }()));
        emit notify(QStringLiteral("载体已生成，共 %1 张").arg(m_carriers.size()));

        // 生成完立即静默上传（三步流程里的中间步骤）
        if (biliConnected()) {
            startPublish();
        } else {
            setBusy(false, {});
            logger::event(QStringLiteral("make.need-bili-login"));
            emit notify(QStringLiteral("请先在账号管理里填写哔哩哔哩 Cookie"));
            emit requestRoute(QStringLiteral("accounts"));
        }
    });

    watcher->setFuture(QtConcurrent::run([=]() {
        MakeResult result;
        result.outputDir = targetDir;

        if (streamMode) {
            const QString streamDir = QDir(cacheDir()).filePath(
                QStringLiteral("stream-carriers/%1").arg(stamp));
            result.ok = prepareQqCarrierPlan(payloadPath,
                                             displayName,
                                             mime,
                                             cover,
                                             streamDir,
                                             &result.qqPlan,
                                             &result.error,
                                             payloadIsBundle,
                                             feijiiMode);
            result.qqStreaming = result.ok && qqMode;
            result.feijiiStreaming = result.ok && feijiiMode;
        } else {
            result.ok = generateCarriers(payloadPath,
                                         displayName,
                                         mime,
                                         cover,
                                         targetDir,
                                         false,
                                         &result.resource,
                                         &result.error,
                                         payloadIsBundle);
        }
        return result;
    }));
}

qint64 AppController::filesTotalBytes() const
{
    qint64 total = 0;
    for (const QVariant &entry : m_files) {
        const QVariantMap item = entry.toMap();
        const qint64 size = item.value(QStringLiteral("size")).toLongLong();
        // 图片项此时已指向压缩产物，size 就是压缩后的体积
        total += size > 0 ? size : QFileInfo(item.value(QStringLiteral("path")).toString()).size();
    }
    return total;
}

qint64 AppController::filesOriginalBytes() const
{
    qint64 total = 0;
    for (const QVariant &entry : m_files) {
        const QVariantMap item = entry.toMap();
        const qint64 original = item.value(QStringLiteral("originalSize")).toLongLong();
        total += original > 0 ? original : item.value(QStringLiteral("size")).toLongLong();
    }
    return total;
}

QString AppController::filesTotalText() const
{
    const qint64 total = filesTotalBytes();
    const qint64 original = filesOriginalBytes();
    if (original > total && total > 0) {
        const int saved = int((1.0 - double(total) / double(original)) * 100.0);
        return QStringLiteral("%1（原 %2，省 %3%）")
            .arg(formatSize(total), formatSize(original))
            .arg(saved);
    }
    return formatSize(total);
}

QVariantList AppController::publishSteps() const
{
    // 依据当前阶段文字判断每一步状态，供界面显示
    const QString stage = m_stage;
    auto done = [&](const QStringList &keys) {
        for (const QString &key : keys) {
            if (!key.isEmpty() && stage.contains(key))
                return true;
        }
        return false;
    };

    QVariantList list;
    auto add = [&](const QString &label, bool isDone, bool active) {
        QVariantMap item;
        item.insert(QStringLiteral("label"), label);
        item.insert(QStringLiteral("done"), isDone);
        item.insert(QStringLiteral("active"), active && !isDone);
        list.append(item);
    };

    if (m_platform == QStringLiteral("小飞机网盘")) {
        const bool uploading = stage.contains(QStringLiteral("上传"))
                               || stage.contains(QStringLiteral("小飞机"));
        const bool sharing = stage.contains(QStringLiteral("永久分享"));
        const bool registering = stage.contains(QStringLiteral("登记口令"));
        add(QStringLiteral("打包并流式加密内容"), uploading || sharing || registering, !uploading);
        add(QStringLiteral("生成加密分片"), uploading || sharing || registering, uploading);
        add(QStringLiteral("上传并创建永久分享"), registering, uploading || sharing);
        add(QStringLiteral("登记取件码"), false, registering);
        return list;
    }

    const bool uploaded = done({QStringLiteral("发布"), QStringLiteral("读取图片直链"), QStringLiteral("登记口令")});
    const bool publishing = done({QStringLiteral("发布到频道"), QStringLiteral("读取图片直链")});
    const bool registering = done({QStringLiteral("登记口令")});
    const bool packing = done({QStringLiteral("上传"), QStringLiteral("发布")});

    add(QStringLiteral("打包并加密内容"), packing || uploaded, !packing);
    add(QStringLiteral("生成载体 PNG"), packing || uploaded, done({QStringLiteral("上传")}));
    add(m_platform == QStringLiteral("哔哩哔哩")
            ? QStringLiteral("上传到哔哩哔哩")
            : QStringLiteral("上传到 QQ 频道"), uploaded, publishing);
    if (m_publishToChannel) {
        add(m_platform == QStringLiteral("哔哩哔哩")
                ? QStringLiteral("发布动态")
                : QStringLiteral("发布频道帖子"), publishing, publishing);
    }
    add(QStringLiteral("登记取件码"), registering, registering);
    return list;
}

void AppController::runUploadProbe(int megabytes)
{
    // 联调探针：用当前登录态上传指定大小的载体，摸清平台上限（CJ_UPLOAD_TEST=<MB>）
    const QString dir = QDir(cacheDir()).filePath(QStringLiteral("probe"));
    QDir().mkpath(dir);
    const QString payload = QDir(dir).filePath(QStringLiteral("probe-%1.bin").arg(megabytes));
    {
        QFile file(payload);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(QByteArray(megabytes * 1024 * 1024, 'P'));
    }

    CreatedResource resource;
    QString error;
    if (!generateCarriers(payload, QStringLiteral("probe.bin"), QStringLiteral("application/octet-stream"),
                          whiteQqCarrierPng(), dir, true, &resource, &error)) {
        logger::event(QStringLiteral("probe.generate-fail"), error);
        QCoreApplication::quit();
        return;
    }
    logger::event(QStringLiteral("probe.upload"), QStringLiteral("mb=%1 file=%2")
                  .arg(megabytes).arg(QFileInfo(resource.parts.first().path).fileName()));
    m_channel->uploadImage(resource.parts.first().path, 0,
                          [megabytes](bool ok, const QString &url, const QString &err) {
        logger::event(ok ? QStringLiteral("probe.ok") : QStringLiteral("probe.fail"),
                      ok ? url.left(200) : err);
        Q_UNUSED(megabytes)
        // 不要在 reply 的 finished 回调里直接退出：让事件循环先清干净
        QTimer::singleShot(1500, qApp, []() { QCoreApplication::quit(); });
    });
}

QString AppController::pythonExecutable() const
{
    const QString fromEnv = qEnvironmentVariable("CJ_PYTHON");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv))
        return fromEnv;
    const QString portable = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("runtime/python/python.exe"));
    if (QFileInfo::exists(portable))
        return portable;
#ifdef CJ_PYTHON_DEFAULT
    const QString bundled = QStringLiteral(CJ_PYTHON_DEFAULT);
    if (QFileInfo::exists(bundled))
        return bundled;
#endif
    return QStandardPaths::findExecutable(QStringLiteral("python"));
}

QString AppController::qqScriptPath() const
{
    return embeddedScriptPath(QStringLiteral("qq_channel.py"));
}

QString AppController::biliScriptPath() const
{
    return embeddedScriptPath(QStringLiteral("bili_dynamic.py"));
}

QString AppController::feijiiScriptPath() const
{
    const QString packaged = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("runtime/python-scripts/feejii.py"));
    if (QFileInfo::exists(packaged))
        return packaged;
    return embeddedScriptPath(QStringLiteral("feejii.py"));
}

QString AppController::joinScriptPath() const
{
    return embeddedScriptPath(QStringLiteral("join_ui.py"));
}

void AppController::startAutoJoin()
{
    if (m_joinProcess)
        return;
    const QString python = pythonExecutable();
    const QString script = joinScriptPath();
    if (python.isEmpty() || script.isEmpty()) {
        logger::event(QStringLiteral("qq.join.skip"), QStringLiteral("missing python/script"));
        emit notify(QStringLiteral("已登录；自动加入频道组件不可用，可稍后手动加入"));
        return;
    }

    m_joinRaw.clear();
    m_joinMessage.clear();
    m_joinSucceeded = false;
    auto *proc = new QProcess(this);
    m_joinProcess = proc;
#ifdef Q_OS_WIN
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    proc->setProgram(python);
    proc->setProcessEnvironment(directUploadEnvironment());
    proc->setArguments(pythonScriptArguments(script, {QStringLiteral("--json")}));
    connect(proc, &QProcess::readyReadStandardOutput, this, [this]() { onAutoJoinOutput(); });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QString text = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        if (!text.isEmpty())
            logger::event(QStringLiteral("qq.join.stderr"), text.left(400));
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onAutoJoinFinished(exitCode); });
    logger::event(QStringLiteral("qq.join.spawn"));
    emit notify(QStringLiteral("正在确认频道成员状态…"));
    proc->start();
}

void AppController::onAutoJoinOutput()
{
    if (!m_joinProcess)
        return;
    m_joinRaw += QString::fromUtf8(m_joinProcess->readAllStandardOutput());
    int newline = -1;
    while ((newline = m_joinRaw.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = m_joinRaw.left(newline).trimmed();
        m_joinRaw.remove(0, newline + 1);
        if (!line.startsWith(QLatin1Char('{')))
            continue;
        const QJsonObject event = QJsonDocument::fromJson(line.toUtf8()).object();
        const QString name = event.value(QStringLiteral("event")).toString();
        if (name == QLatin1String("joined") && event.value(QStringLiteral("joined")).toBool())
            m_joinSucceeded = true;
        if (name == QLatin1String("fail"))
            m_joinMessage = event.value(QStringLiteral("message")).toString();
    }
}

void AppController::onAutoJoinFinished(int exitCode)
{
    if (m_joinProcess) {
        onAutoJoinOutput();
        m_joinProcess->deleteLater();
        m_joinProcess = nullptr;
    }
    if (exitCode == 0 && m_joinSucceeded) {
        logger::event(QStringLiteral("qq.join.ok"));
        emit notify(QStringLiteral("已自动加入指定频道"));
        return;
    }
    const QString message = m_joinMessage.isEmpty()
                                ? QStringLiteral("自动加入频道未完成（退出码 %1）").arg(exitCode)
                                : m_joinMessage;
    logger::event(QStringLiteral("qq.join.fail"), message);
    emit notify(message + QStringLiteral("；不影响当前登录，可稍后重试"));
}

// 哔哩哔哩没有二维码登录入口：用户从浏览器复制 Cookie，本机 DPAPI 加密保存，
// 上传脚本读同一个文件（bili-session.dat），两边格式一致。
bool AppController::biliConnected() const
{
    // pathFor 会自动补 ".dat"，这里只传基名，得到 bili-session.dat
    const QString path = securestore::pathFor(QStringLiteral("bili-session"));
    const bool exists = QFileInfo::exists(path);
    logger::event(QStringLiteral("bili.session.check"),
                  QStringLiteral("path=%1 exists=%2").arg(path).arg(exists ? 1 : 0));
    return exists;
}

void AppController::startFeijiiLogin(bool force)
{
    if (!force && m_feijiiLoginProcess)
        return;
    if (m_feijiiLoginProcess) {
        m_feijiiLoginProcess->disconnect(this);
        m_feijiiLoginProcess->kill();
        m_feijiiLoginProcess->deleteLater();
        m_feijiiLoginProcess = nullptr;
    }
    const QString python = pythonExecutable();
    const QString script = feijiiScriptPath();
    if (python.isEmpty() || script.isEmpty()) {
        m_feijiiState = QStringLiteral("error");
        m_feijiiMessage = QStringLiteral("找不到 Python 或小飞机脚本：%1 / %2").arg(python, script);
        emit feijiiChanged();
        return;
    }

    m_feijiiState = QStringLiteral("loading");
    m_feijiiMessage = QStringLiteral("正在生成小飞机扫码登录二维码…");
    m_feijiiQrPath.clear();
    m_feijiiLoginRaw.clear();
    emit feijiiChanged();

    auto *proc = new QProcess(this);
    m_feijiiLoginProcess = proc;
#ifdef Q_OS_WIN
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    const QString qrPath = QDir(cacheDir()).filePath(QStringLiteral("feijii-login.png"));
    proc->setProgram(python);
    proc->setArguments(pythonScriptArguments(
        script, {QStringLiteral("--json"), QStringLiteral("--qr-output"), qrPath,
                 QStringLiteral("--timeout"), QStringLiteral("180")}));
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        if (proc == m_feijiiLoginProcess)
            onFeijiiLoginOutput();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QString text = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        if (!text.isEmpty())
            logger::event(QStringLiteral("feijii.login.stderr"), text.left(400));
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc](int exitCode, QProcess::ExitStatus) {
                if (proc == m_feijiiLoginProcess)
                    onFeijiiLoginFinished(exitCode);
            });
    logger::event(QStringLiteral("feijii.login.spawn"));
    proc->start();
}

void AppController::onFeijiiLoginOutput()
{
    if (!m_feijiiLoginProcess)
        return;
    m_feijiiLoginRaw += QString::fromUtf8(m_feijiiLoginProcess->readAllStandardOutput());
    int newline = -1;
    while ((newline = m_feijiiLoginRaw.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = m_feijiiLoginRaw.left(newline).trimmed();
        m_feijiiLoginRaw.remove(0, newline + 1);
        if (!line.startsWith(QLatin1Char('{')))
            continue;
        const QJsonObject event = QJsonDocument::fromJson(line.toUtf8()).object();
        const QString name = event.value(QStringLiteral("event")).toString();
        logger::event(QStringLiteral("feijii.login.event"), name);
        if (name == QLatin1String("qr") || name == QLatin1String("qr_ready")) {
            m_feijiiQrPath = event.value(QStringLiteral("png")).toString();
            m_feijiiState = QStringLiteral("waiting");
            m_feijiiMessage = event.value(QStringLiteral("message")).toString();
        } else if (name == QLatin1String("scanned")) {
            m_feijiiState = QStringLiteral("scanned");
            m_feijiiMessage = event.value(QStringLiteral("message")).toString();
        } else if (name == QLatin1String("logged_in")) {
            m_feijiiConnected = true;
            m_settings.setValue(QStringLiteral("feijiiConnected"), true);
            m_feijiiState = QStringLiteral("loggedIn");
            m_feijiiMessage = QStringLiteral("小飞机网盘已登录");
            refreshAccounts();
            emit notify(QStringLiteral("小飞机网盘登录成功"));
        } else if (name == QLatin1String("logged_out")) {
            m_feijiiConnected = false;
            m_settings.setValue(QStringLiteral("feijiiConnected"), false);
            m_feijiiState = QStringLiteral("idle");
            m_feijiiMessage = QStringLiteral("已退出小飞机网盘");
            refreshAccounts();
        } else if (name == QLatin1String("expired") || name == QLatin1String("timeout")) {
            m_feijiiState = QStringLiteral("expired");
            m_feijiiMessage = event.value(QStringLiteral("message")).toString();
        } else if (name == QLatin1String("fail")) {
            m_feijiiState = QStringLiteral("error");
            m_feijiiMessage = event.value(QStringLiteral("message")).toString();
            logger::event(QStringLiteral("feijii.login.fail"), m_feijiiMessage);
        }
        emit feijiiChanged();
    }
}

void AppController::onFeijiiLoginFinished(int exitCode)
{
    if (QProcess *proc = m_feijiiLoginProcess) {
        m_feijiiLoginProcess = nullptr;
        proc->deleteLater();
    }
    if (exitCode != 0 && m_feijiiState != QLatin1String("expired")
        && m_feijiiState != QLatin1String("error")) {
        m_feijiiState = QStringLiteral("error");
        m_feijiiMessage = QStringLiteral("小飞机登录脚本退出（代码 %1）").arg(exitCode);
        emit feijiiChanged();
    }
}

void AppController::setBiliCookie(const QString &cookie)
{
    const QString text = cookie.trimmed();
    if (text.isEmpty()) {
        emit notify(QStringLiteral("Cookie 为空"));
        return;
    }
    if (!text.contains(QStringLiteral("SESSDATA")) || !text.contains(QStringLiteral("bili_jct"))) {
        emit notify(QStringLiteral("Cookie 需要同时包含 SESSDATA 与 bili_jct"));
        return;
    }
    securestore::save(QStringLiteral("bili-session"), text.toUtf8());
    logger::event(QStringLiteral("bili.cookie.saved"), QStringLiteral("length=%1").arg(text.size()));
    refreshAccounts();
    emit notify(QStringLiteral("哔哩哔哩登录信息已保存"));
}

void AppController::clearBiliCookie()
{
    securestore::remove(QStringLiteral("bili-session"));
    logger::event(QStringLiteral("bili.cookie.cleared"));
    refreshAccounts();
    emit notify(QStringLiteral("已退出哔哩哔哩"));
}

// 安卓版用 zxing 本地画二维码，PC 版由脚本用 segno 画，界面只负责显示这个 PNG
void AppController::startBiliLogin(bool force)
{
    // 页面可能被创建两次；已经在扫码就直接复用，只有用户点“刷新”才强制重开
    if (!force && m_biliQrProcess)
        return;
    if (m_biliQrProcess) {
        // 先断开旧进程的全部回调，否则它迟到的 finished 会误改新进程状态
        m_biliQrProcess->disconnect(this);
        m_biliQrProcess->kill();
        m_biliQrProcess->deleteLater();
        m_biliQrProcess = nullptr;
    }
    const QString python = pythonExecutable();
    const QString script = biliScriptPath();
    if (python.isEmpty() || script.isEmpty()) {
        m_biliState = QStringLiteral("error");
        m_biliMessage = QStringLiteral("找不到 Python 或脚本：%1 / %2").arg(python, script);
        emit biliChanged();
        return;
    }
    m_biliState = QStringLiteral("loading");
    m_biliMessage = QStringLiteral("正在获取二维码…");
    m_biliQrPath.clear();
    m_biliQrRaw.clear();
    emit biliChanged();

    auto *proc = new QProcess(this);
    m_biliQrProcess = proc;
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    proc->setProgram(python);
    proc->setArguments(pythonScriptArguments(
        script, {QStringLiteral("--json"), QStringLiteral("qr"), QStringLiteral("--wait")}));
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        if (proc == m_biliQrProcess)
            onBiliQrOutput();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QString text = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
        if (!text.isEmpty())
            logger::event(QStringLiteral("bili.qr.stderr"), text.left(300));
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc](int exitCode, QProcess::ExitStatus) {
                if (proc == m_biliQrProcess)
                    onBiliQrFinished(exitCode);
            });
    logger::event(QStringLiteral("bili.qr.spawn"));
    proc->start();
}

void AppController::onBiliQrOutput()
{
    if (!m_biliQrProcess)
        return;
    logger::event(QStringLiteral("bili.qr.read"));
    m_biliQrRaw += QString::fromUtf8(m_biliQrProcess->readAllStandardOutput());
    int newline = -1;
    while ((newline = m_biliQrRaw.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = m_biliQrRaw.left(newline).trimmed();
        m_biliQrRaw.remove(0, newline + 1);
        if (!line.startsWith(QLatin1Char('{')))
            continue;
        const QJsonObject event = QJsonDocument::fromJson(line.toUtf8()).object();
        const QString name = event.value(QStringLiteral("event")).toString();
        logger::event(QStringLiteral("bili.qr.event"), name);
        if (name == QLatin1String("qr")) {
            m_biliQrPath = event.value(QStringLiteral("png")).toString();
            m_biliState = QStringLiteral("waiting");
            m_biliMessage = QStringLiteral("请用哔哩哔哩手机客户端扫码");
        } else if (name == QLatin1String("poll")) {
            const QString state = event.value(QStringLiteral("state")).toString();
            if (state == QLatin1String("waiting")) {
                m_biliState = QStringLiteral("waiting");
                m_biliMessage = QStringLiteral("请用哔哩哔哩手机客户端扫码");
            } else if (state == QLatin1String("scanned")) {
                m_biliState = QStringLiteral("scanned");
                m_biliMessage = QStringLiteral("已扫码，请在手机上确认登录");
            } else if (state == QLatin1String("confirmed")) {
                m_biliState = QStringLiteral("loggedIn");
                m_biliMessage = QStringLiteral("哔哩哔哩已登录");
                logger::event(QStringLiteral("bili.qr.confirmed"));
                refreshAccounts();
                emit notify(QStringLiteral("哔哩哔哩登录成功"));
            } else if (state == QLatin1String("expired") || state == QLatin1String("timeout")) {
                m_biliState = QStringLiteral("expired");
                m_biliMessage = QStringLiteral("二维码已失效，点“刷新二维码”重来");
            }
        } else if (name == QLatin1String("fail")) {
            m_biliState = QStringLiteral("error");
            m_biliMessage = event.value(QStringLiteral("message")).toString();
            logger::event(QStringLiteral("bili.qr.fail"), m_biliMessage);
        }
        emit biliChanged();
    }
}

void AppController::onBiliQrFinished(int exitCode)
{
    if (QProcess *proc = m_biliQrProcess) {
        m_biliQrProcess = nullptr;
        proc->deleteLater();
    }
    if (m_biliState == QLatin1String("loading") || m_biliState == QLatin1String("waiting")
        || m_biliState == QLatin1String("scanned")) {
        if (exitCode != 0) {
            m_biliState = QStringLiteral("error");
            m_biliMessage = QStringLiteral("登录脚本退出（代码 %1）").arg(exitCode);
        } else if (m_biliState != QLatin1String("loggedIn")) {
            m_biliState = QStringLiteral("expired");
            m_biliMessage = QStringLiteral("二维码已失效，点“刷新二维码”重来");
        }
        emit biliChanged();
    }
}

void AppController::startQqStream(const QqCarrierPlan &plan, const QString &payloadPath)
{
    const QString python = pythonExecutable();
    const QString script = qqScriptPath();
    if (python.isEmpty() || script.isEmpty()) {
        emit notify(QStringLiteral("找不到 Python 或 QQ 上传脚本"));
        return;
    }

    if (m_postProcess) {
        m_postProcess->kill();
        m_postProcess->deleteLater();
        m_postProcess = nullptr;
    }

    m_qqStreamPlan = plan;
    m_qqStreamPayloadPath = payloadPath;
    m_qqStreamCurrentPath.clear();
    m_qqStreamNextIndex = 0;
    m_qqStreamAwaitingIndex = -1;
    m_qqStreaming = true;
    m_created = plan.resource;
    m_postRaw.clear();
    m_postError.clear();
    m_postUrls.clear();
    m_postPosted = false;

    QVector<qint64> totals;
    totals.reserve(plan.plainSizes.size());
    const qint64 whiteBytes = whiteQqCarrierPng().size();
    for (int i = 0; i < plan.plainSizes.size(); ++i) {
        const qint64 imageBytes = i == 0 ? plan.coverImage.size() : whiteBytes;
        const qint64 metaBytes = i == 0
                                     ? plan.resource.encryptedMetadata.size() - kIvSize
                                     : 0;
        totals.append(imageBytes + plan.plainSizes.at(i) + kTagSize + metaBytes + kIndexSize);
    }
    resetProgress(totals);
    setBusy(true, QStringLiteral("正在生成第 1/%1 张载体…").arg(plan.plainSizes.size()));

    auto *proc = new QProcess(this);
    m_postProcess = proc;
#ifdef Q_OS_WIN
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    proc->setProgram(python);
    QStringList args{QStringLiteral("--json"), QStringLiteral("stream")};
    if (!m_publishToChannel)
        args << QStringLiteral("--no-publish");
    proc->setArguments(pythonScriptArguments(script, args));
    connect(proc, &QProcess::started, this, [this]() { generateNextQqCarrier(); });
    connect(proc, &QProcess::readyReadStandardOutput, this, [this]() { onPostOutput(); });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QString text = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
        if (!text.isEmpty())
            logger::event(QStringLiteral("qq.stream.stderr"), text.left(400));
    });
    connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            m_postError = QStringLiteral("QQ 流式上传进程启动失败");
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onPostFinished(exitCode); });
    logger::event(QStringLiteral("qq.stream.spawn"),
                  QStringLiteral("parts=%1 publish=%2 script=%3")
                      .arg(plan.plainSizes.size())
                      .arg(m_publishToChannel ? 1 : 0)
                      .arg(script));
    proc->start();
}

void AppController::startFeijiiStream(const QqCarrierPlan &plan, const QString &payloadPath)
{
    const QString python = pythonExecutable();
    const QString script = feijiiScriptPath();
    if (python.isEmpty() || script.isEmpty()) {
        emit notify(QStringLiteral("找不到 Python 或小飞机上传脚本"));
        return;
    }
    if (m_postProcess) {
        m_postProcess->kill();
        m_postProcess->deleteLater();
        m_postProcess = nullptr;
    }

    m_qqStreamPlan = plan;
    m_qqStreamPayloadPath = payloadPath;
    m_qqStreamCurrentPath.clear();
    m_qqStreamNextIndex = 0;
    m_qqStreamAwaitingIndex = -1;
    m_qqStreaming = false;
    m_feijiiStreaming = true;
    m_feijiiPendingUrl.clear();
    m_created = plan.resource;
    m_postRaw.clear();
    m_postError.clear();
    m_postUrls.clear();
    m_postPosted = false;

    QVector<qint64> totals;
    totals.reserve(plan.plainSizes.size());
    for (int i = 0; i < plan.plainSizes.size(); ++i) {
        const qint64 metaBytes = i == 0 ? plan.resource.encryptedMetadata.size() : 0;
        totals.append(plan.plainSizes.at(i) + 17 + metaBytes + kIndexSize);
    }
    resetProgress(totals);
    setBusy(true, QStringLiteral("正在生成第 1/%1 个加密分片…").arg(plan.plainSizes.size()));
    logger::event(QStringLiteral("feijii.stream.start"),
                  QStringLiteral("parts=%1").arg(plan.plainSizes.size()));
    generateNextQqCarrier();
}

void AppController::startFeijiiUploadPart(const QString &path, int index)
{
    auto *proc = new QProcess(this);
    m_postProcess = proc;
    m_postRaw.clear();
    m_postError.clear();
    m_feijiiPendingUrl.clear();
#ifdef Q_OS_WIN
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    proc->setProgram(pythonExecutable());
    proc->setProcessEnvironment(directUploadEnvironment());
    const QString script = feijiiScriptPath();
    proc->setArguments(pythonScriptArguments(
        script, {QStringLiteral("--json"), QStringLiteral("--upload"), path}));
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        proc->setProperty("cjHadOutput", true);
        onPostOutput();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QString text = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        if (!text.isEmpty())
            logger::event(QStringLiteral("feijii.upload.stderr"), text.left(500));
    });
    connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            m_postError = QStringLiteral("小飞机上传进程启动失败");
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onPostFinished(exitCode); });
    logger::event(QStringLiteral("feijii.upload.spawn"),
                  QStringLiteral("part=%1 path=%2 script=%3")
                      .arg(index + 1).arg(path, script));
    proc->start();
    QTimer::singleShot(30000, proc, [this, proc]() {
        if (proc != m_postProcess || proc->state() == QProcess::NotRunning
            || proc->property("cjHadOutput").toBool()) {
            return;
        }
        m_postError = QStringLiteral(
            "小飞机直连接口 30 秒没有响应；请检查网络和登录状态");
        logger::event(QStringLiteral("feijii.upload.no-output"), m_postError);
        proc->kill();
    });
}

void AppController::generateNextQqCarrier()
{
    if ((!m_qqStreaming && !m_feijiiStreaming) || m_qqStreamAwaitingIndex >= 0)
        return;
    if (m_qqStreaming && !m_postProcess)
        return;
    if (m_qqStreamNextIndex >= m_qqStreamPlan.plainSizes.size()) {
        if (m_qqStreamPayloadPath.startsWith(cacheDir())) {
            QFile::remove(m_qqStreamPayloadPath);
            m_qqStreamPayloadPath.clear();
        }
        if (m_feijiiStreaming) {
            m_feijiiStreaming = false;
            QDir streamDir(m_qqStreamPlan.workDir);
            if (m_qqStreamPlan.workDir.startsWith(cacheDir()))
                streamDir.removeRecursively();
            m_progress = 100;
            m_uploadSent = m_uploadTotal;
            qint64 totalBytes = 0;
            for (qint64 total : std::as_const(m_uploadTotal))
                totalBytes += total;
            m_uploadSummary = QStringLiteral("已上传 %1 / %1（100%）").arg(formatSize(totalBytes));
            emit progressChanged();
            setBusy(true, QStringLiteral("正在登记口令…"));
            finishRegistration(m_postUrls);
            return;
        }
        setBusy(true, m_publishToChannel ? QStringLiteral("正在完成频道发布…")
                                         : QStringLiteral("正在汇总图片直链…"));
        QJsonObject finish;
        finish.insert(QStringLiteral("cmd"), QStringLiteral("finish"));
        m_postProcess->write(QJsonDocument(finish).toJson(QJsonDocument::Compact) + '\n');
        return;
    }

    const int index = m_qqStreamNextIndex;
    setBusy(true, QStringLiteral("正在生成第 %1/%2 张载体…")
                      .arg(index + 1)
                      .arg(m_qqStreamPlan.plainSizes.size()));
    auto *watcher = new QFutureWatcher<QqPartResult>(this);
    connect(watcher, &QFutureWatcher<QqPartResult>::finished, this, [this, watcher, index]() {
        const QqPartResult result = watcher->result();
        watcher->deleteLater();
        if (!result.ok) {
            m_postError = result.error.isEmpty() ? QStringLiteral("生成流式载体失败") : result.error;
            if (m_postProcess) {
                m_postProcess->kill();
            } else {
                m_feijiiStreaming = false;
                setBusy(false, {});
                emit notify(m_postError);
            }
            return;
        }
        if (m_qqStreamPlan.binaryMode)
            m_qqStreamPlan.streamState = result.streamState;

        m_created.parts.append(result.part);
        QVariantMap map;
        map.insert(QStringLiteral("name"), QFileInfo(result.part.path).fileName());
        map.insert(QStringLiteral("path"), result.part.path);
        map.insert(QStringLiteral("sha"), result.part.remoteSha256Hex);
        map.insert(QStringLiteral("size"), QFileInfo(result.part.path).size());
        map.insert(QStringLiteral("meta"), QStringLiteral("等待上传 · %1")
                                                   .arg(formatSize(QFileInfo(result.part.path).size())));
        m_carriers.append(map);
        emit carriersChanged();

        m_qqStreamCurrentPath = result.part.path;
        m_qqStreamAwaitingIndex = index;
        if (!QFileInfo::exists(m_qqStreamCurrentPath)) {
            m_postError = QStringLiteral("刚生成的上传分片已丢失，请关闭其他文件取件实例后重试：%1")
                              .arg(m_qqStreamCurrentPath);
            logger::event(QStringLiteral("stream.part.missing"), m_postError);
            if (m_postProcess)
                m_postProcess->kill();
            else {
                m_feijiiStreaming = false;
                setBusy(false, {});
                emit notify(m_postError);
            }
            return;
        }
        setBusy(true, m_feijiiStreaming
                          ? QStringLiteral("正在上传第 %1/%2 个加密分片…")
                                .arg(index + 1)
                                .arg(m_qqStreamPlan.plainSizes.size())
                          : QStringLiteral("正在上传第 %1/%2 张载体…")
                          .arg(index + 1)
                          .arg(m_qqStreamPlan.plainSizes.size()));
        if (m_feijiiStreaming) {
            startFeijiiUploadPart(result.part.path, index);
            return;
        }
        QJsonObject upload;
        upload.insert(QStringLiteral("cmd"), QStringLiteral("upload"));
        upload.insert(QStringLiteral("path"), result.part.path);
        upload.insert(QStringLiteral("path_b64"),
                      QString::fromLatin1(result.part.path.toUtf8().toBase64(
                          QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
        upload.insert(QStringLiteral("index"), index);
        m_postProcess->write(QJsonDocument(upload).toJson(QJsonDocument::Compact) + '\n');
    });
    QqCarrierPlan plan = m_qqStreamPlan;
    watcher->setFuture(QtConcurrent::run([plan, index]() mutable {
        QqPartResult result;
        result.ok = generateQqCarrierPart(&plan, index, &result.part, &result.error);
        if (result.ok && plan.binaryMode)
            result.streamState = plan.streamState;
        return result;
    }));
}

void AppController::handleQqCarrierUploaded(int index, const QString &url)
{
    if (!m_qqStreaming && !m_feijiiStreaming)
        return;
    if (index != m_qqStreamAwaitingIndex || url.isEmpty()) {
        m_postError = m_feijiiStreaming
                          ? QStringLiteral("小飞机上传返回的分片序号或永久分享链接无效")
                          : QStringLiteral("QQ 上传返回的载体序号或直链无效");
        if (m_postProcess)
            m_postProcess->kill();
        return;
    }

    while (m_postUrls.size() <= index)
        m_postUrls.append(QString());
    m_postUrls[index] = url;

    // 直链已经收到并保存在内存后，才删除本地一次性载体。
    QFile::remove(m_qqStreamCurrentPath);
    m_qqStreamCurrentPath.clear();
    if (index >= 0 && index < m_carriers.size()) {
        QVariantMap map = m_carriers.at(index).toMap();
        map.insert(QStringLiteral("path"), QString());
        map.insert(QStringLiteral("meta"), QStringLiteral("已上传并清理本地载体"));
        m_carriers[index] = map;
        emit carriersChanged();
    }

    m_qqStreamAwaitingIndex = -1;
    m_qqStreamNextIndex = index + 1;
    QTimer::singleShot(0, this, [this]() { generateNextQqCarrier(); });
}

void AppController::startPublish()
{
    if (m_carriers.isEmpty()) {
        emit notify(QStringLiteral("请先生成载体"));
        return;
    }
    const bool bili = m_platform == QStringLiteral("哔哩哔哩");
    if (bili) {
        if (!biliConnected()) {
            emit notify(QStringLiteral("请先在账号管理里填写哔哩哔哩 Cookie"));
            return;
        }
    } else if (!m_qqLogin || !m_qqLogin->loggedIn()) {
        emit notify(QStringLiteral("请先登录 QQ 频道"));
        return;
    }

    QStringList files;
    for (const QVariant &entry : std::as_const(m_carriers))
        files << entry.toMap().value(QStringLiteral("path")).toString();

    const QString python = pythonExecutable();
    const QString script = bili ? biliScriptPath() : qqScriptPath();
    if (python.isEmpty() || script.isEmpty()) {
        logger::event(QStringLiteral("api.post.nopython"),
                      QStringLiteral("python=%1 script=%2").arg(python, script));
        emit notify(QStringLiteral("找不到 Python 或上传脚本：%1 / %2").arg(python, script));
        return;
    }

    QVector<qint64> uploadTotals;
    uploadTotals.reserve(files.size());
    for (const QString &file : std::as_const(files))
        uploadTotals.append(qMax<qint64>(1, QFileInfo(file).size()));
    resetProgress(uploadTotals);
    setBusy(true, bili
                      ? QStringLiteral("正在上传到哔哩哔哩…")
                      : QStringLiteral("正在上传到 QQ 频道…"));
    logger::event(QStringLiteral("api.publish.begin"),
                  QStringLiteral("files=%1 via=python publish=%2")
                      .arg(files.size())
                      .arg(m_publishToChannel ? 1 : 0));

    if (m_postProcess) {
        m_postProcess->kill();
        m_postProcess->deleteLater();
        m_postProcess = nullptr;
    }
    m_postRaw.clear();
    m_postError.clear();
    m_postUrls.clear();
    m_postPosted = false;

    auto *proc = new QProcess(this);
    m_postProcess = proc;
    // python 是控制台程序，不加这个每张图都会闪一个 cmd 黑窗
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    proc->setProgram(python);
    proc->setProcessEnvironment(directUploadEnvironment());
    QStringList args{QStringLiteral("--json"), QStringLiteral("post")};
    for (const QString &file : std::as_const(files))
        args << QStringLiteral("--file") << file;
    if (!m_publishToChannel)
        args << QStringLiteral("--no-publish");
    proc->setArguments(pythonScriptArguments(script, args));

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        proc->setProperty("cjHadOutput", true);
        onPostOutput();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QString text = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        if (!text.isEmpty())
            logger::event(QStringLiteral("api.post.stderr"), text.left(800));
    });
    connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            m_postError = QStringLiteral("Python 启动失败");
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onPostFinished(exitCode); });

    logger::event(QStringLiteral("api.post.spawn"),
                  QStringLiteral("noPublish=%1 · %2 %3")
                      .arg(m_publishToChannel ? 0 : 1)
                      .arg(python, args.join(QLatin1Char(' ')).left(300)));
    proc->start();
    QTimer::singleShot(30000, proc, [this, proc, bili]() {
        if (proc != m_postProcess || proc->state() == QProcess::NotRunning
            || proc->property("cjHadOutput").toBool()) {
            return;
        }
        m_postError = bili
                          ? QStringLiteral("哔哩哔哩上传脚本启动后 30 秒没有响应，请检查网络和登录状态")
                          : QStringLiteral("QQ 上传脚本启动后 30 秒没有响应，请检查网络和登录状态");
        logger::event(QStringLiteral("api.post.no-output"), m_postError);
        proc->kill();
    });
}

void AppController::onPostOutput()
{
    if (!m_postProcess)
        return;
    m_postRaw += QString::fromUtf8(m_postProcess->readAllStandardOutput());
    int newline = -1;
    while ((newline = m_postRaw.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = m_postRaw.left(newline).trimmed();
        m_postRaw.remove(0, newline + 1);
        if (!line.startsWith(QLatin1Char('{')))
            continue;
        const QJsonObject event = QJsonDocument::fromJson(line.toUtf8()).object();
        const QString name = event.value(QStringLiteral("event")).toString();
        if (name == QLatin1String("slice") || name == QLatin1String("progress")) {
            const QString sentKey = name == QLatin1String("slice")
                                        ? QStringLiteral("done") : QStringLiteral("sent");
            onChannelProgress(event.value(QStringLiteral("index")).toInt(),
                              static_cast<qint64>(event.value(sentKey).toDouble()),
                              static_cast<qint64>(qMax(1, event.value(QStringLiteral("total")).toInt())));
        } else if (name == QLatin1String("upload_progress")) {
            onChannelProgress(m_qqStreamAwaitingIndex,
                              static_cast<qint64>(event.value(QStringLiteral("loadedBytes")).toDouble()),
                              qMax<qint64>(1, static_cast<qint64>(event.value(QStringLiteral("totalBytes")).toDouble())));
            const QString stageLabel = event.value(QStringLiteral("stageLabel")).toString();
            if (!stageLabel.isEmpty())
                setBusy(true, QStringLiteral("小飞机：%1（第 %2/%3 片）")
                                  .arg(stageLabel)
                                  .arg(m_qqStreamAwaitingIndex + 1)
                                  .arg(m_qqStreamPlan.plainSizes.size()));
        } else if (name == QLatin1String("upload_preparing")) {
            setBusy(true, QStringLiteral("正在准备上传第 %1/%2 个加密分片…")
                              .arg(m_qqStreamAwaitingIndex + 1)
                              .arg(m_qqStreamPlan.plainSizes.size()));
        } else if (name == QLatin1String("upload_complete")) {
            const qint64 total = qMax<qint64>(1, static_cast<qint64>(event.value(QStringLiteral("totalBytes")).toDouble()));
            onChannelProgress(m_qqStreamAwaitingIndex, total, total);
            setBusy(true, QStringLiteral("分片上传完成，正在创建永久分享…"));
        } else if (name == QLatin1String("share_creating")) {
            setBusy(true, QStringLiteral("正在为第 %1/%2 个分片创建永久分享…")
                              .arg(m_qqStreamAwaitingIndex + 1)
                              .arg(m_qqStreamPlan.plainSizes.size()));
        } else if (name == QLatin1String("share_complete")) {
            m_feijiiPendingUrl = event.value(QStringLiteral("url")).toString().trimmed();
            setBusy(true, QStringLiteral("永久分享已创建，正在清理本地分片…"));
        } else if (name == QLatin1String("uploading")) {
            setBusy(true, m_platform == QStringLiteral("哔哩哔哩")
                              ? QStringLiteral("正在上传到哔哩哔哩…")
                              : QStringLiteral("正在上传到 QQ 频道…"));
        } else if (name == QLatin1String("verifying")) {
            setBusy(true, QStringLiteral("正在等待哔哩哔哩确认上传…"));
        } else if (name == QLatin1String("uploaded")) {
            if (m_qqStreaming) {
                handleQqCarrierUploaded(event.value(QStringLiteral("index")).toInt(-1),
                                        event.value(QStringLiteral("url")).toString());
            } else {
                setBusy(true, QStringLiteral("上传完成，正在继续…"));
            }
        } else if (name == QLatin1String("posted")) {
            m_postPosted = true;
            m_postUrls.clear();
            const QJsonArray urls = event.value(QStringLiteral("urls")).toArray();
            for (const QJsonValue &value : urls)
                m_postUrls << value.toString();
            logger::event(QStringLiteral("api.post.ok"),
                          QStringLiteral("images=%1 published=%2")
                              .arg(m_postUrls.size())
                              .arg(event.value(QStringLiteral("published")).toBool(true) ? 1 : 0));
            if (!event.value(QStringLiteral("published")).toBool(true))
                setBusy(true, QStringLiteral("仅上传完成，正在登记取件码…"));
        } else if (name == QLatin1String("publishing")) {
            const int batch = event.value(QStringLiteral("batch")).toInt();
            setBusy(true, m_platform == QStringLiteral("哔哩哔哩")
                              ? QStringLiteral("正在发布哔哩哔哩动态…")
                              : batch > 0
                                    ? QStringLiteral("正在发布第 %1 条 QQ 频道作品…").arg(batch)
                                    : QStringLiteral("正在发布 QQ 频道帖子…"));
        } else if (name == QLatin1String("published_batch")) {
            setBusy(true, QStringLiteral("第 %1 条频道作品已发布，继续处理载体…")
                              .arg(event.value(QStringLiteral("batch")).toInt()));
        } else if (name == QLatin1String("fail") || name == QLatin1String("error")
                   || name == QLatin1String("upload_failed")
                   || name == QLatin1String("upload_timeout")) {
            const QString message = event.value(QStringLiteral("message")).toString();
            if (!message.isEmpty() && m_postError.isEmpty())
                m_postError = message;
        }
    }
}

void AppController::onPostFinished(int exitCode)
{
    // finished 可能先于最后一次 readyRead 信号被处理；先消费尾部 JSON，避免
    // 小飞机已返回永久链接却因 m_feijiiPendingUrl 尚未更新而被误判失败。
    onPostOutput();
    if (m_feijiiStreaming) {
        const int index = m_qqStreamAwaitingIndex;
        const QString url = m_feijiiPendingUrl;
        if (m_postProcess) {
            m_postProcess->deleteLater();
            m_postProcess = nullptr;
        }
        if (exitCode != 0 || !m_postError.isEmpty() || url.isEmpty()) {
            m_feijiiStreaming = false;
            setBusy(false, {});
            const QString message = !m_postError.isEmpty()
                                        ? m_postError
                                        : QStringLiteral("小飞机上传或创建永久分享失败（退出码 %1）").arg(exitCode);
            logger::event(QStringLiteral("feijii.upload.fail"), message);
            emit notify(message);
            return;
        }
        m_feijiiPendingUrl.clear();
        handleQqCarrierUploaded(index, url);
        return;
    }
    const bool wasQqStreaming = m_qqStreaming;
    m_qqStreaming = false;
    m_qqStreamAwaitingIndex = -1;
    if (m_postProcess) {
        m_postProcess->deleteLater();
        m_postProcess = nullptr;
    }
    if (!m_postError.isEmpty() || !m_postPosted || m_postUrls.isEmpty()) {
        setBusy(false, {});
        const QString message = m_postError.isEmpty()
                                    ? QStringLiteral("上传脚本失败（退出码 %1）").arg(exitCode)
                                    : m_postError;
        logger::event(QStringLiteral("api.post.fail"), message);
        emit notify(message);
        return;
    }

    if (wasQqStreaming) {
        if (!m_qqStreamPayloadPath.isEmpty() && m_qqStreamPayloadPath.startsWith(cacheDir()))
            QFile::remove(m_qqStreamPayloadPath);
        m_qqStreamPayloadPath.clear();
        QDir streamDir(m_qqStreamPlan.workDir);
        if (m_qqStreamPlan.workDir.startsWith(cacheDir()))
            streamDir.removeRecursively();
    }

    m_progress = 100;
    m_uploadDone = m_uploadTotal.size();
    qint64 totalBytes = 0;
    for (qint64 total : std::as_const(m_uploadTotal))
        totalBytes += total;
    m_uploadSent = m_uploadTotal;
    m_uploadSummary = QStringLiteral("已上传 %1 / %1（100%）").arg(formatSize(totalBytes));
    emit progressChanged();
    setBusy(true, QStringLiteral("正在登记口令…"));
    finishRegistration(m_postUrls);
}

void AppController::finishRegistration(const QStringList &urls)
{
    m_backend->registerResource(
        m_created, urls, [this](bool registered, const QString &code, const QString &error) {
            setBusy(false, {});
            if (!registered) {
                logger::event(QStringLiteral("register.fail"), error);
                emit notify(error.isEmpty() ? QStringLiteral("口令登记失败") : error);
                return;
            }
            m_code = code;
            emit codeChanged();
            const QString countKey = m_platform == QStringLiteral("小飞机网盘")
                                         ? QStringLiteral("feijiiCount")
                                         : m_platform == QStringLiteral("哔哩哔哩")
                                               ? QStringLiteral("biliCount")
                                               : QStringLiteral("qqCount");
            m_settings.setValue(countKey, m_settings.value(countKey, 0).toInt() + 1);
            refreshAccounts();
            logger::event(QStringLiteral("register.ok"), QStringLiteral("codeLength=%1").arg(code.size()));
            appendHistory(displayNameOfFiles(), QStringLiteral("制作"), code, true, {},
                          m_created.originalSize);
            emit notify(QStringLiteral("口令已生成"));
        });
}

void AppController::resetProgress(const QVector<qint64> &totals)
{
    m_uploadDone = 0;
    m_progress = 0;
    m_uploadSent = QVector<qint64>(totals.size(), 0);
    m_uploadTotal = totals;
    qint64 totalBytes = 0;
    for (qint64 total : totals)
        totalBytes += total;
    m_uploadSummary = totals.isEmpty()
                          ? QString()
                          : QStringLiteral("已上传 0 B / %1（0%）").arg(formatSize(totalBytes));
    emit progressChanged();
}

void AppController::onChannelProgress(int index, qint64 sent, qint64 total)
{
    if (index < 0 || index >= m_uploadSent.size())
        return;
    if (total > 0)
        m_uploadTotal[index] = total;
    const qint64 slotTotal = qMax<qint64>(1, m_uploadTotal.at(index));
    m_uploadSent[index] = qBound<qint64>(0, sent, slotTotal);

    qint64 sumSent = 0;
    qint64 sumTotal = 0;
    for (int i = 0; i < m_uploadSent.size(); ++i) {
        sumSent += m_uploadSent.at(i);
        sumTotal += m_uploadTotal.at(i);
    }
    const int percent = sumTotal > 0 ? static_cast<int>(sumSent * 100 / sumTotal) : 0;

    int finished = 0;
    for (int i = 0; i < m_uploadTotal.size(); ++i) {
        if (m_uploadTotal.at(i) > 0 && m_uploadSent.at(i) >= m_uploadTotal.at(i))
            ++finished;
    }
    m_uploadDone = finished;
    m_progress = percent;
    m_uploadSummary = QStringLiteral("已上传 %1 / %2（%3%）")
                          .arg(formatSize(sumSent), formatSize(sumTotal))
                          .arg(percent);
    m_stage = m_platform == QStringLiteral("哔哩哔哩")
                  ? QStringLiteral("正在上传到哔哩哔哩…")
                  : m_platform == QStringLiteral("小飞机网盘")
                        ? QStringLiteral("正在上传到小飞机网盘…")
                        : QStringLiteral("正在上传到 QQ 频道…");
    emit progressChanged();
    emit stageChanged();
}

void AppController::resetDownloadProgress(const ResourceRecord &record)
{
    QVector<qint64> totals;
    totals.reserve(record.parts.size());
    qint64 totalBytes = 0;
    for (const ResourcePart &part : record.parts) {
        const qint64 bytes = qMax<qint64>(1, part.cipherSize);
        totals.append(bytes);
        totalBytes += bytes;
    }
    m_uploadDone = 0;
    m_progress = 0;
    m_uploadSent = QVector<qint64>(totals.size(), 0);
    m_uploadTotal = totals;
    m_uploadSummary = totals.isEmpty()
                          ? QString()
                          : QStringLiteral("已下载 0 B / %1（0%）").arg(formatSize(totalBytes));
    emit progressChanged();
}

void AppController::onDownloadProgress(int index, qint64 received, qint64 total)
{
    if (index < 0 || index >= m_uploadSent.size() || total <= 0)
        return;
    const qint64 expected = qMax<qint64>(1, m_uploadTotal.at(index));
    m_uploadSent[index] = qBound<qint64>(qint64(0), expected * received / total, expected);

    qint64 sumReceived = 0;
    qint64 sumTotal = 0;
    int finished = 0;
    for (int i = 0; i < m_uploadSent.size(); ++i) {
        sumReceived += m_uploadSent.at(i);
        sumTotal += m_uploadTotal.at(i);
        if (m_uploadSent.at(i) >= m_uploadTotal.at(i))
            ++finished;
    }
    const int percent = sumTotal > 0 ? int(sumReceived * 100 / sumTotal) : 0;
    m_uploadDone = finished;
    m_progress = percent;
    m_uploadSummary = QStringLiteral("已下载 %1 / %2（%3%）")
                          .arg(formatSize(sumReceived), formatSize(sumTotal))
                          .arg(percent);
    m_stage = QStringLiteral("正在下载内容…");
    emit progressChanged();
    emit stageChanged();
}

void AppController::verifyLocal()
{
    if (m_carriers.isEmpty()) {
        emit notify(QStringLiteral("请先生成载体"));
        return;
    }
    QStringList paths;
    for (const QVariant &entry : std::as_const(m_carriers))
        paths << entry.toMap().value(QStringLiteral("path")).toString();

    ResourceRecord record;
    record.protocolVersion = 1;
    record.fileKey = m_created.fileKey;
    record.encryptedMetadata = m_created.encryptedMetadata;
    record.originalSha256Hex = m_created.originalSha256Hex;

    const QString workDir = QDir(cacheDir()).filePath(QStringLiteral("restored"));
    RestoredResult restored;
    QString error;
    if (!restoreFromCarriers(paths, record, workDir, &restored, &error)) {
        emit notify(error.isEmpty() ? QStringLiteral("回读校验失败") : error);
        return;
    }

    m_outputs.clear();
    if (restored.children.isEmpty()) {
        QVariantMap map;
        map.insert(QStringLiteral("name"), restored.name);
        map.insert(QStringLiteral("path"), restored.path);
        map.insert(QStringLiteral("kind"), kindOf(restored.path));
        map.insert(QStringLiteral("size"), QFileInfo(restored.path).size());
        map.insert(QStringLiteral("meta"), QStringLiteral("%1 · %2")
                                               .arg(restored.mime,
                                                    formatSize(QFileInfo(restored.path).size())));
        m_outputs.append(map);
    } else {
        for (const BundleItem &child : std::as_const(restored.children)) {
            QVariantMap map;
            map.insert(QStringLiteral("name"), child.name);
            map.insert(QStringLiteral("path"), child.path);
            map.insert(QStringLiteral("kind"), kindOf(child.path));
            map.insert(QStringLiteral("size"), child.size);
            map.insert(QStringLiteral("meta"),
                       QStringLiteral("%1 · %2").arg(mimeOf(child.path), formatSize(child.size)));
            m_outputs.append(map);
        }
    }
    emit outputsChanged();
    emit notify(QStringLiteral("本机回读校验通过：%1").arg(restored.name));
}

QString AppController::displayNameOfFiles() const
{
    if (m_files.size() == 1)
        return m_files.first().toMap().value(QStringLiteral("name")).toString();
    return QStringLiteral("%1 项内容").arg(m_files.size());
}

void AppController::pasteFromClipboard()
{
    const QString text = QGuiApplication::clipboard()->text();
    setCode(text);
    if (m_code.size() != 5) {
        emit notify(QStringLiteral("剪贴板中没有足够的英文字母组成 5 位口令"));
        return;
    }
    emit notify(QStringLiteral("已从剪贴板读取口令"));
}

void AppController::copyCode()
{
    if (m_code.size() != 5)
        return;
    QGuiApplication::clipboard()->setText(m_code);
    emit notify(QStringLiteral("口令已复制"));
}

void AppController::copyText(const QString &text)
{
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    emit notify(QStringLiteral("已复制"));
}

QString AppController::clipboardText()
{
    return QGuiApplication::clipboard()->text();
}

void AppController::startExtract()
{
    if (m_busy)
        return;
    static const QRegularExpression codePattern(QStringLiteral("^[A-Z]{5}$"));
    if (!codePattern.match(m_code).hasMatch()) {
        emit notify(QStringLiteral("请输入 5 位英文口令"));
        return;
    }

    resetProgress({});
    setBusy(true, QStringLiteral("正在读取口令记录…"));
    logger::event(QStringLiteral("extract.begin"), QStringLiteral("code=%1").arg(m_code));
    m_backend->fetchRecord(m_code, [this](bool ok, const ResourceRecord &record, const QString &error) {
        if (!ok) {
            logger::event(QStringLiteral("extract.getfail"), error);
            setBusy(false, {});
            emit notify(error.isEmpty() ? QStringLiteral("口令读取失败") : error);
            appendHistory(m_code, QStringLiteral("提取"), m_code, false);
            return;
        }

        if (record.protocolVersion == 2) {
            RestoredResult restored;
            QString inlineError;
            const bool inlineOk = restoreInlineText(record,
                                                    newExtractSessionDir(),
                                                    &restored,
                                                    &inlineError);
            setBusy(false, {});
            if (!inlineOk) {
                emit notify(inlineError.isEmpty() ? QStringLiteral("提取失败") : inlineError);
                appendHistory(m_code, QStringLiteral("提取"), m_code, false);
                return;
            }
            m_outputs.clear();
            QVariantMap map;
            map.insert(QStringLiteral("name"), restored.name);
            map.insert(QStringLiteral("path"), restored.path);
            map.insert(QStringLiteral("kind"), QStringLiteral("文字"));
            map.insert(QStringLiteral("size"), QFileInfo(restored.path).size());
            map.insert(QStringLiteral("meta"), QStringLiteral("text/plain · %1")
                                                   .arg(formatSize(QFileInfo(restored.path).size())));
            m_outputs.append(map);
            emit outputsChanged();
            appendHistory(restored.name, QStringLiteral("提取"), m_code, true, m_extractSessionDir,
                          QFileInfo(restored.path).size());
            emit notify(QStringLiteral("提取完成"));
            return;
        }

        logger::event(QStringLiteral("extract.record"),
                      QStringLiteral("protocol=%1 parts=%2 keylen=%3")
                          .arg(record.protocolVersion).arg(record.parts.size())
                          .arg(record.fileKey.size()));
        if (record.protocolVersion != 1) {
            setBusy(false, {});
            emit notify(QStringLiteral("当前版本仅支持图片平台口令"));
            return;
        }
        if (record.parts.isEmpty()) {
            setBusy(false, {});
            emit notify(QStringLiteral("口令记录中没有分片"));
            return;
        }
        resetDownloadProgress(record);

        // 同一个口令再提取：先比对内容指纹，没变就提示“已提取过”
        const QJsonObject previous = m_extractIndex.value(m_code).toObject();
        if (!previous.isEmpty()) {
            const QString previousSha = previous.value(QStringLiteral("sha")).toString();
            const QString previousDir = previous.value(QStringLiteral("dir")).toString();
            const QString previousTime = previous.value(QStringLiteral("time")).toString();
            if (previousSha == record.originalSha256Hex) {
                if (!m_forceExtract && !m_forceOnce) {
                    setBusy(false, {});
                    logger::event(QStringLiteral("extract.duplicate"),
                                  QStringLiteral("code=%1").arg(m_code));
                    m_duplicateMessage =
                        QStringLiteral("口令 %1 在 %2 已经提取过了。\n"
                                       "内容与当时完全一致（SHA-256 未变），无需重新下载。")
                            .arg(m_code, previousTime);
                    m_duplicateDir = previousDir;
                    m_duplicatePrompt = true;
                    emit duplicatePromptChanged();
                    return;
                }
                m_forceOnce = false;
                logger::event(QStringLiteral("extract.duplicate.forced"), QStringLiteral("code=%1").arg(m_code));
            } else {
                emit notify(QStringLiteral("内容与上次提取时不同（可能已重新制作），按新内容恢复"));
            }
        }

        const QString dir = QDir(cacheDir()).filePath(
            QStringLiteral("download-%1").arg(QDateTime::currentMSecsSinceEpoch()));
        QDir().mkpath(dir);

        auto paths = QSharedPointer<QStringList>::create();
        auto index = QSharedPointer<int>::create(0);
        auto self = QSharedPointer<std::function<void()>>::create();
        auto fail = [this](const QString &message) {
            setBusy(false, {});
            emit notify(message);
            appendHistory(m_code, QStringLiteral("提取"), m_code, false);
        };

        const QString downloadDir = dir;
        *self = [this, record, downloadDir, dir, paths, index, self, fail]() {
            if (*index >= record.parts.size()) {
                setBusy(true, QStringLiteral("正在校验并恢复…"));
                RestoredResult restored;
                QString restoreError;
                const bool ok = restoreFromCarriers(*paths,
                                                    record,
                                                    newExtractSessionDir(),
                                                    &restored,
                                                    &restoreError);
                setBusy(false, {});
                // 分片是临时下载的，恢复完就删，别在缓存里攒着
                QDir(downloadDir).removeRecursively();
                logger::event(ok ? QStringLiteral("extract.restore.ok") : QStringLiteral("extract.restore.fail"),
                              ok ? QStringLiteral("parts=%1").arg(paths->size()) : restoreError);
                if (!ok) {
                    fail(restoreError.isEmpty() ? QStringLiteral("恢复失败") : restoreError);
                    return;
                }
                m_outputs.clear();
                if (restored.children.isEmpty()) {
                    QVariantMap map;
                    map.insert(QStringLiteral("name"), restored.name);
                    map.insert(QStringLiteral("path"), restored.path);
                    map.insert(QStringLiteral("kind"), kindOf(restored.path));
                    map.insert(QStringLiteral("size"), QFileInfo(restored.path).size());
                    map.insert(QStringLiteral("meta"),
                               QStringLiteral("%1 · %2")
                                   .arg(restored.mime, formatSize(QFileInfo(restored.path).size())));
                    m_outputs.append(map);
                } else {
                    for (const BundleItem &child : std::as_const(restored.children)) {
                        QVariantMap map;
                        map.insert(QStringLiteral("name"), child.name);
                        map.insert(QStringLiteral("path"), child.path);
                        map.insert(QStringLiteral("kind"), kindOf(child.path));
                        map.insert(QStringLiteral("size"), child.size);
                        map.insert(QStringLiteral("meta"), QStringLiteral("%1 · %2")
                                                               .arg(mimeOf(child.path),
                                                                    formatSize(child.size)));
                        m_outputs.append(map);
                    }
                }
                emit outputsChanged();
                qint64 restoredBytes = 0;
                if (restored.children.isEmpty()) {
                    restoredBytes = QFileInfo(restored.path).size();
                } else {
                    for (const BundleItem &child : std::as_const(restored.children))
                        restoredBytes += child.size;
                }
                appendHistory(restored.name, QStringLiteral("提取"), m_code, true, m_extractSessionDir,
                              restoredBytes);
                rememberExtraction(m_code, record.originalSha256Hex, restoredBytes);
                emit notify(QStringLiteral("提取完成"));
                return;
            }

            const ResourcePart part = record.parts.at(*index);
            const QString target =
                QDir(dir).filePath(QStringLiteral("part-%1.png").arg(part.partIndex));
            setBusy(true, QStringLiteral("正在下载内容…"));

            logger::event(QStringLiteral("extract.download"),
                          QStringLiteral("idx=%1 url=%2").arg(part.partIndex).arg(part.downloadUrl.left(160)));
            const int progressIndex = *index;
            m_backend->downloadFile(part.downloadUrl, target, [this, part, target, paths, index, self, fail](
                                                                  bool ok, const QString &error) {
                if (!ok) {
                    logger::event(QStringLiteral("extract.download.fail"), error);
                    fail(error.isEmpty() ? QStringLiteral("分片下载失败") : error);
                    return;
                }
                const QString actual = cj::hex(cj::sha256File(target));
                logger::event(QStringLiteral("extract.download.ok"),
                              QStringLiteral("idx=%1 bytes=%2").arg(part.partIndex).arg(QFileInfo(target).size()));
                if (!part.remoteSha256Hex.isEmpty()
                    && actual.compare(part.remoteSha256Hex, Qt::CaseInsensitive) != 0) {
                    logger::event(QStringLiteral("extract.sha.fail"),
                                  QStringLiteral("idx=%1 expect=%2 actual=%3")
                                      .arg(part.partIndex).arg(part.remoteSha256Hex.left(16), actual.left(16)));
                    fail(QStringLiteral("第 %1 个分片远端校验失败").arg(part.partIndex + 1));
                    return;
                }
                paths->append(target);
                ++(*index);
                (*self)();
            }, [this, progressIndex](qint64 received, qint64 total) {
                onDownloadProgress(progressIndex, received, total);
            });
        };

        (*self)();
    });
}

void AppController::resetExtract()
{
    m_code.clear();
    m_outputs.clear();
    emit codeChanged();
    emit outputsChanged();
}

void AppController::appendHistory(const QString &name,
                                  const QString &kind,
                                  const QString &code,
                                  bool ok,
                                  const QString &dir,
                                  qint64 bytes)
{
    QVariantMap entry;
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("kind"), kind);
    entry.insert(QStringLiteral("code"), code);
    if (!dir.isEmpty())
        entry.insert(QStringLiteral("dir"), dir);
    if (bytes > 0)
        entry.insert(QStringLiteral("size"), bytes);
    entry.insert(QStringLiteral("time"),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    entry.insert(QStringLiteral("ok"), ok);
    m_history.prepend(entry);
    while (m_history.size() > 100)
        m_history.removeLast();

    QJsonArray array;
    for (const QVariant &item : std::as_const(m_history))
        array.append(QJsonObject::fromVariantMap(item.toMap()));
    m_settings.setValue(QStringLiteral("history"),
                        QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    emit historyChanged();
}

void AppController::loadHistory()
{
    const QString raw = m_settings.value(QStringLiteral("history")).toString();
    if (raw.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isArray())
        return;
    m_history.clear();
    for (const QJsonValue &value : doc.array())
        m_history.append(value.toObject().toVariantMap());
}

void AppController::clearHistory()
{
    m_history.clear();
    m_settings.remove(QStringLiteral("history"));
    emit historyChanged();
    emit notify(QStringLiteral("已清空历史记录"));
}

void AppController::setLongLink(const QString &platform, bool value)
{
    const QString key = platform == QStringLiteral("哔哩哔哩") ? QStringLiteral("biliLongLink")
                                                               : QStringLiteral("qqLongLink");
    m_settings.setValue(key, value);
}

void AppController::logoutPlatform(const QString &platform)
{
    if (platform == QStringLiteral("小飞机网盘")) {
        if (m_feijiiLoginProcess)
            return;
        const QString python = pythonExecutable();
        const QString script = feijiiScriptPath();
        if (python.isEmpty() || script.isEmpty()) {
            emit notify(QStringLiteral("找不到小飞机退出登录脚本"));
            return;
        }
        m_feijiiState = QStringLiteral("loading");
        m_feijiiMessage = QStringLiteral("正在退出小飞机网盘…");
        m_feijiiLoginRaw.clear();
        emit feijiiChanged();
        auto *proc = new QProcess(this);
        m_feijiiLoginProcess = proc;
#ifdef Q_OS_WIN
        proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif
        proc->setProgram(python);
        proc->setArguments(pythonScriptArguments(
            script, {QStringLiteral("--json"), QStringLiteral("--logout")}));
        connect(proc, &QProcess::readyReadStandardOutput, this, [this]() { onFeijiiLoginOutput(); });
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus) { onFeijiiLoginFinished(exitCode); });
        proc->start();
        return;
    }
    if (platform == QStringLiteral("哔哩哔哩")) {
        clearBiliCookie();
        return;
    }
    emit notify(QStringLiteral("%1 当前阶段尚未接入登录，无需退出").arg(platform));
}

QString AppController::screenshotPath() const
{
    return qEnvironmentVariable("CJ_SHOT");
}

QString AppController::initialRoute() const
{
    return qEnvironmentVariable("CJ_ROUTE");
}

bool AppController::demoMode() const
{
    return qEnvironmentVariableIsSet("CJ_DEMO");
}

void AppController::openLog()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(logger::path()));
}

void AppController::checkForUpdates()
{
    if (m_updateChecking)
        return;
    m_updateChecking = true;
    emit updateChanged();
    logger::event(QStringLiteral("update.check.begin"), QCoreApplication::applicationVersion());
    m_backend->checkClientUpdate(
        QCoreApplication::applicationVersion(),
        [this](bool ok, const ClientUpdateInfo &info, const QString &error) {
            m_updateChecking = false;
            if (!ok) {
                emit updateChanged();
                logger::event(QStringLiteral("update.check.fail"), error);
                emit notify(error.isEmpty() ? QStringLiteral("检查更新失败，请稍后重试") : error);
                return;
            }
            if (!info.available) {
                emit updateChanged();
                logger::event(QStringLiteral("update.check.current"),
                              QCoreApplication::applicationVersion());
                emit notify(QStringLiteral("当前已是最新版"));
                return;
            }
            m_updateVersion = info.version;
            m_updateNotes = info.notes.isEmpty() ? QStringLiteral("本次更新未提供说明") : info.notes;
            m_updateSizeText = humanSize(info.size);
            m_updateDownloadUrl = info.downloadUrl;
            emit updateChanged();
            logger::event(QStringLiteral("update.check.available"),
                          QStringLiteral("version=%1 bytes=%2").arg(info.version).arg(info.size));
            emit updateReady();
        });
}

void AppController::openUpdateDownload()
{
    const QUrl url(m_updateDownloadUrl);
    if (!url.isValid() || url.scheme() != QStringLiteral("https")) {
        emit notify(QStringLiteral("更新下载地址无效"));
        return;
    }
    QDesktopServices::openUrl(url);
}

QString AppController::logPath() const
{
    return logger::path();
}

void AppController::saveWindowSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    m_windowWidth = width;
    m_windowHeight = height;
    m_settings.setValue(QStringLiteral("windowWidth"), width);
    m_settings.setValue(QStringLiteral("windowHeight"), height);
}

void AppController::demoFill()
{
    // 仅用于联调截图：写入一小段示例文字
    const QString dir = QDir(cacheDir()).filePath(QStringLiteral("demo"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("示例文本.txt"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QStringLiteral("这是文件取件 PC 版的示例隐藏内容。\n用于验证载体生成与回读校验。").toUtf8());
        file.close();
    }
    QVariantMap item;
    item.insert(QStringLiteral("name"), QStringLiteral("示例文本.txt"));
    item.insert(QStringLiteral("path"), path);
    item.insert(QStringLiteral("kind"), QStringLiteral("文字"));
    item.insert(QStringLiteral("size"), static_cast<qint64>(QFileInfo(path).size()));
    item.insert(QStringLiteral("meta"),
                QStringLiteral("文字 · %1").arg(formatSize(QFileInfo(path).size())));
    m_files.clear();
    m_files.append(item);
    emit filesChanged();
}

bool AppController::longLink(const QString &platform)
{
    const QString key = platform == QStringLiteral("哔哩哔哩") ? QStringLiteral("biliLongLink")
                                                               : QStringLiteral("qqLongLink");
    return m_settings.value(key, true).toBool();
}

void AppController::refreshAccounts()
{
    m_accounts.clear();
    QVariantMap feijii;
    feijii.insert(QStringLiteral("name"), QStringLiteral("小飞机网盘"));
    feijii.insert(QStringLiteral("recommended"), true);
    feijii.insert(QStringLiteral("connected"), m_feijiiConnected);
    feijii.insert(QStringLiteral("count"), m_settings.value(QStringLiteral("feijiiCount"), 0).toInt());
    feijii.insert(QStringLiteral("detail"), QStringLiteral("本地流式加密，上传后创建永久分享"));
    m_accounts.append(feijii);

    QVariantMap qq;
    qq.insert(QStringLiteral("name"), QStringLiteral("QQ 频道"));
    qq.insert(QStringLiteral("recommended"), false);
    qq.insert(QStringLiteral("connected"), m_qqLogin && m_qqLogin->loggedIn());
    qq.insert(QStringLiteral("count"), m_settings.value(QStringLiteral("qqCount"), 0).toInt());
    qq.insert(QStringLiteral("detail"), QStringLiteral("发布到固定频道 pd55235600"));
    m_accounts.append(qq);

    QVariantMap bili;
    bili.insert(QStringLiteral("name"), QStringLiteral("哔哩哔哩"));
    bili.insert(QStringLiteral("recommended"), false);
    bili.insert(QStringLiteral("connected"), biliConnected());
    bili.insert(QStringLiteral("count"), m_settings.value(QStringLiteral("biliCount"), 0).toInt());
    bili.insert(QStringLiteral("detail"), QStringLiteral("通过哔哩哔哩发放内容"));
    m_accounts.append(bili);
    emit accountsChanged();
}

} // namespace cj
