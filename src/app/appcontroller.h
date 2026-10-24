#pragma once

#include "core/backend.h"
#include "core/creator.h"
#include "core/restorer.h"
#include "qq/channelapi.h"
#include "qq/qrlogin.h"

#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QDir>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class QFutureWatcherBase;
class QProcess;

namespace cj {

// QML 侧的单一入口：设置、内容选择、制作、提取、历史与账号状态。
class AppController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY settingsChanged)
    Q_PROPERTY(bool compressImages READ compressImages WRITE setCompressImages NOTIFY settingsChanged)
    Q_PROPERTY(bool fullCheck READ fullCheck WRITE setFullCheck NOTIFY settingsChanged)
    Q_PROPERTY(QString platform READ platform WRITE setPlatform NOTIFY settingsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString stage READ stage NOTIFY stageChanged)
    // 压缩结果摘要，形如 "webp-q75 · 12.3 MB → 3.4 MB（省 72%）"，空串表示未压缩
    Q_PROPERTY(QString compressInfo READ compressInfo NOTIFY compressInfoChanged)
    // 导入即转换的进度：compressBusy 为真时界面显示独立进度条
    Q_PROPERTY(bool compressBusy READ compressBusy NOTIFY compressProgressChanged)
    Q_PROPERTY(int compressProgress READ compressProgress NOTIFY compressProgressChanged)
    // 网络传输进度：0-100；uploadSummary 显示总体已传输/总字节，不暴露分片细节
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString uploadSummary READ uploadSummary NOTIFY progressChanged)
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    // 待发送总量：图片用压缩后的体积，其它文件按原体积，合计才是真正要上传的大小
    Q_PROPERTY(qint64 filesTotalBytes READ filesTotalBytes NOTIFY filesChanged)
    Q_PROPERTY(qint64 filesOriginalBytes READ filesOriginalBytes NOTIFY filesChanged)
    Q_PROPERTY(QString filesTotalText READ filesTotalText NOTIFY filesChanged)
    Q_PROPERTY(QVariantList carriers READ carriers NOTIFY carriersChanged)
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY outputsChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(bool feijiiConnected READ feijiiConnected NOTIFY feijiiChanged)
    Q_PROPERTY(QString feijiiQrPath READ feijiiQrPath NOTIFY feijiiChanged)
    Q_PROPERTY(QString feijiiState READ feijiiState NOTIFY feijiiChanged)
    Q_PROPERTY(QString feijiiMessage READ feijiiMessage NOTIFY feijiiChanged)
    Q_PROPERTY(bool biliConnected READ biliConnected NOTIFY accountsChanged)
    Q_PROPERTY(QString biliQrPath READ biliQrPath NOTIFY biliChanged)
    Q_PROPERTY(QString biliState READ biliState NOTIFY biliChanged)
    Q_PROPERTY(QString biliMessage READ biliMessage NOTIFY biliChanged)
    Q_PROPERTY(QString code READ code WRITE setCode NOTIFY codeChanged)
    Q_PROPERTY(QString outputDir READ outputDir NOTIFY outputDirChanged)
    Q_PROPERTY(QString extractDir READ extractDir NOTIFY extractDirChanged)
    Q_PROPERTY(QString extractSessionDir READ extractSessionDir NOTIFY extractSessionDirChanged)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY outputsChanged)
    Q_PROPERTY(int windowWidth READ windowWidth NOTIFY windowStateChanged)
    Q_PROPERTY(int windowHeight READ windowHeight NOTIFY windowStateChanged)
    Q_PROPERTY(cj::QqQrLogin *qqLogin READ qqLogin CONSTANT)
    Q_PROPERTY(cj::ChannelApi *channel READ channel CONSTANT)
    Q_PROPERTY(bool publishToChannel READ publishToChannel WRITE setPublishToChannel NOTIFY settingsChanged)
    Q_PROPERTY(QString screenshotPath READ screenshotPath CONSTANT)
    Q_PROPERTY(QString initialRoute READ initialRoute CONSTANT)
    Q_PROPERTY(bool demoMode READ demoMode CONSTANT)
    Q_PROPERTY(bool updateChecking READ updateChecking NOTIFY updateChanged)
    Q_PROPERTY(QString updateVersion READ updateVersion NOTIFY updateChanged)
    Q_PROPERTY(QString updateNotes READ updateNotes NOTIFY updateChanged)
    Q_PROPERTY(QString updateSizeText READ updateSizeText NOTIFY updateChanged)

public:
    explicit AppController(QObject *parent = nullptr);

    bool darkMode() const { return m_dark; }
    void setDarkMode(bool value);
    bool compressImages() const { return m_compressImages; }
    void setCompressImages(bool value);
    bool fullCheck() const { return m_fullCheck; }
    void setFullCheck(bool value);
    QString platform() const { return m_platform; }
    void setPlatform(const QString &value);

    bool busy() const { return m_busy; }
    QString stage() const { return m_stage; }
    int progress() const { return m_progress; }
    QString uploadSummary() const { return m_uploadSummary; }
    QString compressInfo() const { return m_compressInfo; }
    bool compressBusy() const { return m_compressBusy; }
    int compressProgress() const { return m_compressProgress; }
    QVariantList files() const { return m_files; }
    qint64 filesTotalBytes() const;
    qint64 filesOriginalBytes() const;
    QString filesTotalText() const;
    QVariantList carriers() const { return m_carriers; }
    QVariantList outputs() const { return m_outputs; }
    QVariantList history() const { return m_history; }
    QVariantList accounts() const { return m_accounts; }
    QString code() const { return m_code; }
    void setCode(const QString &value);
    QString outputDir() const { return m_outputDir; }
    QString extractDir() const { return m_extractDir; }
    QString extractSessionDir() const { return m_extractSessionDir; }
    Q_INVOKABLE void setExtractDir(const QString &value);
    Q_INVOKABLE void openExtractDir();
    Q_INVOKABLE QString newExtractSessionDir();
    Q_INVOKABLE void clearExtractDir();
    bool hasResult() const { return !m_outputs.isEmpty(); }
    int windowWidth() const { return m_windowWidth; }
    int windowHeight() const { return m_windowHeight; }
    QqQrLogin *qqLogin() const { return m_qqLogin; }
    ChannelApi *channel() const { return m_channel; }
    bool publishToChannel() const { return m_publishToChannel; }
    void setPublishToChannel(bool value);
    QString screenshotPath() const;
    QString initialRoute() const;
    bool demoMode() const;
    bool updateChecking() const { return m_updateChecking; }
    QString updateVersion() const { return m_updateVersion; }
    QString updateNotes() const { return m_updateNotes; }
    QString updateSizeText() const { return m_updateSizeText; }

    // 内容选择
    Q_INVOKABLE void addUrls(const QStringList &urls);
    Q_INVOKABLE void addText(const QString &text);
    Q_INVOKABLE void removeAt(int index);
    Q_INVOKABLE void clearFiles();
    Q_INVOKABLE QString readTextFile(const QString &path);
    Q_INVOKABLE QString humanSize(qint64 bytes) const;
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE void revealPath(const QString &path);

    // 制作：打包 → 生成载体 → 落到输出目录
    Q_INVOKABLE void startMake();
    Q_INVOKABLE void resetMake();
    Q_INVOKABLE void openOutputDir();

    // 本机回读校验（不经过平台，验证载体可恢复）
    Q_INVOKABLE void verifyLocal();

    // 提取：口令 → 后端记录 → 下载分片 → 校验 → 恢复
    Q_INVOKABLE void startPublish();
    void runUploadProbe(int megabytes);
    // 发放步骤清单（给界面显示进度用）
    Q_INVOKABLE QVariantList publishSteps() const;
    Q_INVOKABLE void startExtract();
    // 忽略「已提取过」的提示，强制重新下载（默认 false）
    Q_INVOKABLE void setForceExtract(bool value);
    // 重复提取弹窗：继续提取 / 打开之前文件夹 / 关闭
    Q_PROPERTY(bool duplicatePrompt READ duplicatePrompt NOTIFY duplicatePromptChanged)
    Q_PROPERTY(QString duplicateMessage READ duplicateMessage NOTIFY duplicatePromptChanged)
    Q_PROPERTY(bool duplicateHasDir READ duplicateHasDir NOTIFY duplicatePromptChanged)
    Q_INVOKABLE void continueAfterDuplicate();
    Q_INVOKABLE void openDuplicateDir();
    Q_INVOKABLE void dismissDuplicatePrompt();
    Q_INVOKABLE bool forceExtract() const { return m_forceExtract; }
    Q_INVOKABLE void resetExtract();
    Q_INVOKABLE void pasteFromClipboard();
    Q_INVOKABLE void copyCode();
    Q_INVOKABLE void copyText(const QString &text);
    Q_INVOKABLE QString clipboardText();

    Q_INVOKABLE void clearHistory();
    Q_INVOKABLE void demoFill();
    Q_INVOKABLE void saveWindowSize(int width, int height);
    Q_INVOKABLE void openLog();
    Q_INVOKABLE QString logPath() const;
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void openUpdateDownload();
    Q_INVOKABLE void setLongLink(const QString &platform, bool value);
    Q_INVOKABLE bool longLink(const QString &platform);
    Q_INVOKABLE void logoutPlatform(const QString &platform);
    Q_INVOKABLE void startFeijiiLogin(bool force = false);
    bool feijiiConnected() const { return m_feijiiConnected; }
    QString feijiiQrPath() const { return m_feijiiQrPath; }
    QString feijiiState() const { return m_feijiiState; }
    QString feijiiMessage() const { return m_feijiiMessage; }
    // 哔哩哔哩没有二维码登录：用户从浏览器复制 Cookie，本机 DPAPI 加密保存
    Q_INVOKABLE void setBiliCookie(const QString &cookie);
    Q_INVOKABLE void clearBiliCookie();
    Q_INVOKABLE void startBiliLogin(bool force = false);
    bool biliConnected() const;
    QString biliQrPath() const { return m_biliQrPath; }
    QString biliState() const { return m_biliState; }
    QString biliMessage() const { return m_biliMessage; }


signals:
    void settingsChanged();
    void busyChanged();
    void stageChanged();
    void compressInfoChanged();
    void compressProgressChanged();
    void progressChanged();
    void filesChanged();
    void carriersChanged();
    void outputsChanged();
    void historyChanged();
    void accountsChanged();
    void feijiiChanged();
    void biliChanged();
    void codeChanged();
    void outputDirChanged();
    void extractDirChanged();
    void extractSessionDirChanged();
    void duplicatePromptChanged();
    void windowStateChanged();
    void updateChanged();
    void updateReady();
    void notify(const QString &message);
    void requestRoute(const QString &route);

private:
    void setBusy(bool value, const QString &stage = {});
    // 导入即转换：把待处理的图片排进后台任务，逐个转成 WebP 并回写列表
    void compressPendingFiles();
    void scheduleFilesChanged();
    void onChannelProgress(int index, qint64 sent, qint64 total);
    void resetDownloadProgress(const ResourceRecord &record);
    void onDownloadProgress(int index, qint64 received, qint64 total);
    // 上传/发帖走 Python 脚本（qq_channel.py），进度按 JSON 行回流
    QString pythonExecutable() const;
    QString qqScriptPath() const;
    QString biliScriptPath() const;
    QString feijiiScriptPath() const;
    QString joinScriptPath() const;
    void startAutoJoin();
    void onAutoJoinOutput();
    void onAutoJoinFinished(int exitCode);
    void onPostOutput();
    void onPostFinished(int exitCode);
    void startQqStream(const QqCarrierPlan &plan, const QString &payloadPath);
    void startFeijiiStream(const QqCarrierPlan &plan, const QString &payloadPath);
    void startFeijiiUploadPart(const QString &path, int index);
    void generateNextQqCarrier();
    void handleQqCarrierUploaded(int index, const QString &url);
    void finishRegistration(const QStringList &urls);
    void resetProgress(const QVector<qint64> &totals);
    QString displayNameOfFiles() const;
    void appendHistory(const QString &name,
                       const QString &kind,
                       const QString &code,
                       bool ok,
                       const QString &dir = {},
                       qint64 bytes = 0);
    void loadSettings();
    void loadHistory();
    void refreshAccounts();
    QString cacheDir();
    QString makeWorkDir();

    QSettings m_settings;
    bool m_dark = false;
    bool m_compressImages = true;
    bool m_fullCheck = true;
    QString m_platform = QStringLiteral("小飞机网盘");
    bool m_busy = false;
    QString m_stage;
    QVariantList m_files;
    QVariantList m_carriers;
    QVariantList m_outputs;
    QVariantList m_history;
    QVariantList m_accounts;
    QString m_code;
    QString m_outputDir;
    QString m_extractDir;
    QString m_extractSessionDir;
    bool m_forceExtract = false;
    bool m_forceOnce = false;
    bool m_duplicatePrompt = false;
    QString m_duplicateMessage;
    QString m_duplicateDir;
    bool duplicatePrompt() const { return m_duplicatePrompt; }
    QString duplicateMessage() const { return m_duplicateMessage; }
    bool duplicateHasDir() const { return !m_duplicateDir.isEmpty() && QDir(m_duplicateDir).exists(); }
    QJsonObject m_extractIndex;   // 口令 → { sha, size, time, dir }，用于判断内容有没有变
    void rememberExtraction(const QString &code, const QString &sha, qint64 bytes);
    void pruneCache();
    int m_windowWidth = 1100;
    int m_windowHeight = 700;
    bool m_updateChecking = false;
    QString m_updateVersion;
    QString m_updateNotes;
    QString m_updateSizeText;
    QString m_updateDownloadUrl;
    QPointer<Backend> m_backend;
    QqQrLogin *m_qqLogin = nullptr;
    ChannelApi *m_channel = nullptr;
    bool m_publishToChannel = false;
    CreatedResource m_created;

    // 上传进度汇总（每张载体一个槽位）
    int m_progress = 0;
    QString m_uploadSummary;
    QVector<qint64> m_uploadSent;
    QVector<qint64> m_uploadTotal;
    int m_uploadDone = 0;

    // 发帖子进程状态
    QProcess *m_postProcess = nullptr;
    QProcess *m_joinProcess = nullptr;
    QString m_joinRaw;
    QString m_joinMessage;
    bool m_joinSucceeded = false;
    QProcess *m_feijiiLoginProcess = nullptr;
    bool m_feijiiConnected = false;
    QString m_feijiiQrPath;
    QString m_feijiiState = QStringLiteral("idle");
    QString m_feijiiMessage;
    QString m_feijiiLoginRaw;
    void onFeijiiLoginOutput();
    void onFeijiiLoginFinished(int exitCode);
    // 哔哩哔哩扫码登录：脚本自己轮询，事件按 JSON 行回流
    QProcess *m_biliQrProcess = nullptr;
    QString m_biliQrPath;
    QString m_biliState = QStringLiteral("idle");
    QString m_biliMessage;
    QString m_biliQrRaw;
    void onBiliQrOutput();
    void onBiliQrFinished(int exitCode);
    QString m_postRaw;
    QString m_postError;
    QStringList m_postUrls;
    QqCarrierPlan m_qqStreamPlan;
    QString m_qqStreamPayloadPath;
    QString m_qqStreamCurrentPath;
    int m_qqStreamNextIndex = 0;
    int m_qqStreamAwaitingIndex = -1;
    bool m_qqStreaming = false;
    bool m_feijiiStreaming = false;
    QString m_feijiiPendingUrl;
    QString m_compressInfo;
    bool m_compressBusy = false;
    bool m_makeAfterCompress = false;
    int m_compressProgress = 0;
    int m_compressDone = 0;
    int m_compressConverted = 0;
    qint64 m_compressBefore = 0;
    qint64 m_compressAfter = 0;
    QStringList m_compressNotes;
    bool m_filesFlushScheduled = false;
    bool m_postPosted = false;
};

} // namespace cj
