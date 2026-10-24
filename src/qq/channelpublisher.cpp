#include "qq/channelpublisher.h"

#include "core/logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QApplication>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebEngineCookieStore>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QWidget>

#include <functional>

namespace cj {

namespace {

// 频道页选择器：集中一处，改版只动这里
constexpr const char *kEditorContainer = ".publish-editor-container";
constexpr const char *kUploadArea = ".upload-area";
constexpr const char *kPublishButton = ".publish-button button";

constexpr int kPollIntervalMs = 1500;
constexpr int kMaxPollTicks = 160;

QString cacheDir()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString dir = QDir(root).filePath(QStringLiteral("channel"));
    QDir().mkpath(dir);
    return dir;
}

} // namespace

// 允许程序给页面的 <input type=file> 提供本地文件（由真实点击触发）
class InjectPage : public QWebEnginePage
{
public:
    InjectPage(QWebEngineProfile *profile, ChannelPublisher *owner)
        : QWebEnginePage(profile, owner)
    {
        Q_UNUSED(owner)
    }

    std::function<QStringList()> fileProvider;

protected:
    QStringList chooseFiles(FileSelectionMode, const QStringList &, const QStringList &) override
    {
        const QStringList files = fileProvider ? fileProvider() : QStringList();
        logger::event(QStringLiteral("publish.chooseFiles"),
                      QStringLiteral("count=%1").arg(files.size()));
        return files;
    }
};

ChannelPublisher::ChannelPublisher(QObject *parent)
    : QObject(parent)
{
    m_profile = new QWebEngineProfile(QStringLiteral("cangjian-channel"), this);
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::AllowPersistentCookies);
    m_profile->setHttpUserAgent(QStringLiteral(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/131.0.0.0 Safari/537.36"));

    // 窗口在首次发布时创建（见 ensureWindow）
}

ChannelPublisher::~ChannelPublisher()
{
    if (m_window) {
        m_window->hide();
        delete m_window;
        m_window = nullptr;
    }
}


void ChannelPublisher::ensureWindow()
{
    if (m_window)
        return;

    m_window = new QWidget();
    m_window->setWindowTitle(QStringLiteral("文件取件 · QQ 频道上传"));
    m_window->resize(1000, 760);
    m_window->move(160, 120);

    auto *layout = new QVBoxLayout(m_window);
    layout->setContentsMargins(0, 0, 0, 0);

    m_view = new QWebEngineView(m_window);
    layout->addWidget(m_view);

    auto *page = new InjectPage(m_profile, this);
    page->fileProvider = [this]() {
        const QStringList files = m_pendingFiles;
        m_pendingFiles.clear();
        return files;
    };
    m_view->setPage(page);
    m_page = page;

    page->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    page->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
}

void ChannelPublisher::showWindow(bool visible)
{
    if (!m_window)
        return;
    if (visible) {
        m_window->move(120, 120);
        m_window->show();
    } else {
        m_window->hide();
    }
    logger::event(QStringLiteral("publish.window"),
                  visible ? QStringLiteral("shown") : QStringLiteral("hidden"));
}

void ChannelPublisher::setSessionCookies(const QList<QNetworkCookie> &cookies)
{
    if (!m_profile || !m_profile->cookieStore())
        return;
    for (const QNetworkCookie &cookie : cookies)
        m_profile->cookieStore()->setCookie(cookie);
    logger::event(QStringLiteral("publish.cookies"), QStringLiteral("count=%1").arg(cookies.size()));
}

void ChannelPublisher::setState(const QString &state, const QString &message)
{
    const bool dirty = m_state != state;
    m_state = state;
    if (m_message != message) {
        m_message = message;
        emit messageChanged();
    }
    if (dirty)
        emit stateChanged();
    emit progress(message);
}

void ChannelPublisher::start(const QStringList &files, bool publish, const QString &channelUrl)
{
    if (files.isEmpty()) {
        emit failed(QStringLiteral("没有可上传的载体"));
        return;
    }
    m_files = files;
    m_publish = publish;
    m_channelUrl = channelUrl;
    m_pollTicks = 0;
    m_clickedPublish = 0;
    logger::event(QStringLiteral("publish.start"),
                  QStringLiteral("files=%1 publish=%2 url=%3")
                      .arg(files.size())
                      .arg(publish ? 1 : 0)
                      .arg(channelUrl));
    ensureWindow();
    if (m_window) {
        m_window->show();
    }
    setState(QStringLiteral("loading"), QStringLiteral("正在打开 QQ 频道…"));
    stepOpenChannel();
}

void ChannelPublisher::abort()
{
    m_pendingFiles.clear();
    setState(QStringLiteral("idle"), QStringLiteral("已取消"));
}

void ChannelPublisher::runJs(const QString &script, std::function<void(const QVariant &)> callback)
{
    m_page->runJavaScript(script, [callback](const QVariant &value) {
        if (callback)
            callback(value);
    });
}

void ChannelPublisher::stepOpenChannel()
{
    disconnect(m_page, &QWebEnginePage::loadFinished, this, nullptr);
    connect(m_page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        if (!ok) {
            setState(QStringLiteral("error"), QStringLiteral("频道页加载失败"));
            emit failed(m_message);
            return;
        }
        logger::event(QStringLiteral("publish.page-loaded"), m_page->url().toString());
        QTimer::singleShot(3000, this, &ChannelPublisher::stepOpenEditor);
    });
    m_page->setUrl(QUrl(m_channelUrl));
}

void ChannelPublisher::stepOpenEditor()
{
    setState(QStringLiteral("editor"), QStringLiteral("正在打开发布编辑器…"));

    const QString script = QStringLiteral(R"JS(
(function () {
    var editor = document.querySelector("%1");
    if (editor && editor.offsetParent !== null) return "editor-ready";
    var wanted = ["发帖", "发布", "说点什么", "创作", "发动态"];
    var nodes = Array.from(document.querySelectorAll("button, [role=button], div[class*=post], div[class*=publish]"));
    for (var i = 0; i < nodes.length; ++i) {
        var t = (nodes[i].innerText || "").trim();
        if (!t || t.length > 12) continue;
        for (var k = 0; k < wanted.length; ++k) {
            if (t.indexOf(wanted[k]) >= 0) { nodes[i].click(); return "clicked:" + t; }
        }
    }
    return "not-found";
})()
)JS").arg(QString::fromLatin1(kEditorContainer));

    runJs(script, [this](const QVariant &value) {
        const QString result = value.toString();
        logger::event(QStringLiteral("publish.editor"), result.left(40));
        if (result == QStringLiteral("not-found")) {
            if (++m_pollTicks < 6) {
                QTimer::singleShot(2000, this, &ChannelPublisher::stepOpenEditor);
                return;
            }
            const QString path = dumpPage();
            setState(QStringLiteral("error"),
                     QStringLiteral("找不到发布入口，已导出页面：%1").arg(path));
            emit failed(m_message);
            return;
        }
        m_pollTicks = 0;
        QTimer::singleShot(2500, this, &ChannelPublisher::stepClickUpload);
    });
}

bool ChannelPublisher::clickAtCssPoint(qreal x, qreal y)
{
    if (!m_view)
        return false;

    // 必须走 QTest（平台层事件注入），Chromium 才会把点击当成真实用户操作
    QWidget *target = m_view->focusProxy() ? m_view->focusProxy() : m_view;
    if (m_window) {
    }
    const QPoint local(qRound(x), qRound(y));
    QTest::mouseMove(target, local);
    QTest::mouseClick(target, Qt::LeftButton, Qt::KeyboardModifiers(), local, 40);
    logger::event(QStringLiteral("publish.click-at"),
                  QStringLiteral("x=%1 y=%2").arg(local.x()).arg(local.y()));
    return true;
}

void ChannelPublisher::stepClickUpload()
{
    setState(QStringLiteral("upload"), QStringLiteral("正在上传 %1 张载体…").arg(m_files.size()));
    m_pendingFiles = m_files;

    // 必须让窗口可见并拿到焦点，Chromium 才接受注入的鼠标事件
    if (m_window) {
        m_window->show();
    }
    if (m_view)
        m_view->setFocus(Qt::MouseFocusReason);

    const QString script = QStringLiteral(R"JS(
(function () {
    function rectOf(el) {
        if (!el) return null;
        var r = el.getBoundingClientRect();
        if (!r || r.width < 4 || r.height < 4) return null;
        return { x: r.left + r.width / 2, y: r.top + r.height / 2,
                 w: r.width, h: r.height,
                 tag: el.tagName, cls: (el.className || "").toString().slice(0, 40) };
    }

    // 1) 精确找上传图标（xlink:href 里带 add-upload）
    var uses = Array.from(document.querySelectorAll("svg use"));
    for (var i = 0; i < uses.length; ++i) {
        var href = uses[i].getAttribute("xlink:href") || uses[i].getAttribute("href") || "";
        if (href.indexOf("add-upload") < 0) continue;
        var box = uses[i].closest("div") || uses[i].parentElement;
        var rect = rectOf(box);
        if (rect) return JSON.stringify(rect);
    }

    // 2) 退路：上传区里的按钮 / 图标容器
    var fallbacks = [".upload-area .upload-button", ".upload-area button", ".upload-area .icon-add-upload", ".upload-area"];
    for (var k = 0; k < fallbacks.length; ++k) {
        var el = document.querySelector(fallbacks[k]);
        var r2 = rectOf(el);
        if (r2) return JSON.stringify(r2);
    }
    return "";
})()
)JS").arg(QString::fromLatin1(kUploadArea), QString::fromLatin1(kEditorContainer));

    runJs(script, [this](const QVariant &value) {
        const QString raw = value.toString();
        if (raw.isEmpty()) {
            const QString path = dumpPage();
            setState(QStringLiteral("error"),
                     QStringLiteral("找不到上传区域，已导出页面：%1").arg(path));
            emit failed(m_message);
            return;
        }
        const QJsonObject object = QJsonDocument::fromJson(raw.toUtf8()).object();
        const qreal x = object.value(QStringLiteral("x")).toDouble();
        const qreal y = object.value(QStringLiteral("y")).toDouble();
        logger::event(QStringLiteral("publish.upload-rect"),
                      QStringLiteral("x=%1 y=%2 w=%3 el=%4.%5")
                          .arg(int(x))
                          .arg(int(y))
                          .arg(int(object.value(QStringLiteral("w")).toDouble()))
                          .arg(object.value(QStringLiteral("tag")).toString())
                          .arg(object.value(QStringLiteral("cls")).toString()));

        // 真实鼠标点击：Chromium 才会触发文件选择框，chooseFiles 提供文件
        clickAtCssPoint(x, y);
        m_pollTicks = 0;
        QTimer::singleShot(kPollIntervalMs, this, &ChannelPublisher::pollPreview);
    });
}

QStringList ChannelPublisher::previewUrls(const QVariant &value) const
{
    QStringList urls;
    const QVariantList list = value.toList();
    for (const QVariant &item : list) {
        const QString url = item.toString();
        if (!url.isEmpty())
            urls.append(url);
    }
    return urls;
}

void ChannelPublisher::pollPreview()
{
    if (++m_pollTicks > kMaxPollTicks) {
        const QString path = dumpPage();
        setState(QStringLiteral("error"), QStringLiteral("上传超时，页面已导出：%1").arg(path));
        emit failed(m_message);
        return;
    }

    const QString script = QStringLiteral(R"JS(
(function () {
    var imgs = Array.from(document.querySelectorAll(".preview-list img"));
    var urls = imgs.map(function (e) { return e.currentSrc || e.src || ""; }).filter(Boolean);
    var input = document.querySelector(".upload-area input[type=file]") || document.querySelector("input[type=file]");
    var files = input ? input.files.length : -1;
    return JSON.stringify({ files: files, urls: urls });
})()
)JS");

    runJs(script, [this](const QVariant &value) {
        const QJsonObject object = QJsonDocument::fromJson(value.toString().toUtf8()).object();
        const int fileCount = object.value(QStringLiteral("files")).toInt(-1);
        QStringList urls;
        int remoteCount = 0;
        for (const QJsonValue &item : object.value(QStringLiteral("urls")).toArray()) {
            const QString url = item.toString();
            urls.append(url);
            if (url.contains(QStringLiteral("photo.store.qq.com")))
                ++remoteCount;
        }

        if (m_pollTicks % 4 == 0)
            logger::event(QStringLiteral("publish.upload-poll"),
                          QStringLiteral("tick=%1 inputFiles=%2 previews=%3 remote=%4")
                              .arg(m_pollTicks)
                              .arg(fileCount)
                              .arg(urls.size())
                              .arg(remoteCount));

        // 必须同时满足：input 里真的拿到文件 + 页面出现图床地址
        const bool inputOk = fileCount >= m_files.size();
        if (inputOk && remoteCount >= m_files.size()) {
            if (m_publish)
                stepPublish();
            else
                stepCollectLinks();
            return;
        }

        if (m_pollTicks == 10)
            logger::event(QStringLiteral("publish.dump"), dumpPage());
        QTimer::singleShot(kPollIntervalMs, this, &ChannelPublisher::pollPreview);
    });
}

void ChannelPublisher::stepPublish()
{
    setState(QStringLiteral("publishing"), QStringLiteral("正在发布到频道…"));

    const QString script = QStringLiteral(R"JS(
(function () {
    var channelBtn = document.querySelector(".chose-channel-btn");
    var channelText = channelBtn ? (channelBtn.innerText || "").trim() : "";
    if (channelBtn && channelText.indexOf("请选择") >= 0) {
        channelBtn.click();
        return "need-channel";
    }
    var btn = document.querySelector("%1");
    if (!btn) return "no-button";
    if (btn.disabled) return "disabled";
    btn.click();
    return "published";
})()
)JS").arg(QString::fromLatin1(kPublishButton));

    runJs(script, [this](const QVariant &value) {
        const QString result = value.toString();
        logger::event(QStringLiteral("publish.click"), result);

        if (result == QStringLiteral("need-channel")) {
            const QString pick = QStringLiteral(R"JS(
(function () {
    var panel = document.querySelector(".chose-channel-panel");
    if (!panel) return "no-panel";
    var items = Array.from(panel.querySelectorAll("*"))
        .filter(function (el) { return (el.innerText || "").trim().length > 0 && el.children.length === 0; });
    if (!items.length) return "no-option";
    items[0].click();
    return "picked:" + (items[0].innerText || "").trim().slice(0, 20);
})()
)JS");
            runJs(pick, [this](const QVariant &picked) {
                logger::event(QStringLiteral("publish.channel"), picked.toString());
                QTimer::singleShot(1500, this, &ChannelPublisher::stepPublish);
            });
            return;
        }

        if (result == QStringLiteral("no-button") || result == QStringLiteral("disabled")) {
            if (++m_clickedPublish < 5) {
                QTimer::singleShot(1500, this, &ChannelPublisher::stepPublish);
                return;
            }
            const QString path = dumpPage();
            setState(QStringLiteral("error"),
                     QStringLiteral("发布按钮不可用（%1），页面已导出：%2").arg(result, path));
            emit failed(m_message);
            return;
        }
        QTimer::singleShot(2500, this, [this]() { verifyPublish(0); });
    });
}

void ChannelPublisher::verifyPublish(int attempt)
{
    const QString script = QStringLiteral(R"JS(
(function () {
    var editor = document.querySelector(".publish-editor-container");
    var editorOpen = !!(editor && editor.offsetParent !== null);
    var posts = Array.from(document.querySelectorAll("a[href*='/post/']"))
        .map(function (a) { return a.href; }).slice(0, 3);
    var texts = Array.from(document.querySelectorAll(".g-dialog, [class*=modal], [class*=toast]"))
        .filter(function (el) { return el.offsetParent !== null; })
        .map(function (el) { return (el.innerText || "").trim().slice(0, 40); })
        .filter(Boolean).slice(0, 4);
    return JSON.stringify({ editorOpen: editorOpen, posts: posts, texts: texts });
})()
)JS");

    runJs(script, [this, attempt](const QVariant &value) {
        const QString raw = value.toString();
        logger::event(QStringLiteral("publish.verify"),
                      QStringLiteral("attempt=%1 %2").arg(attempt).arg(raw.left(220)));

        if (!raw.contains(QStringLiteral("\"editorOpen\":true"))) {
            setState(QStringLiteral("published"), QStringLiteral("已发布到频道"));
            stepCollectLinks();
            return;
        }

        if (attempt < 3) {
            const QString click = QStringLiteral(R"JS(
(function () {
    var btns = Array.from(document.querySelectorAll("button"));
    for (var i = 0; i < btns.length; ++i) {
        var t = (btns[i].innerText || "").trim();
        if (btns[i].offsetParent !== null && (t === "确认" || t === "确定" || t === "继续发布" || t === "发布")) {
            btns[i].click();
            return "clicked:" + t;
        }
    }
    return "none";
})()
)JS");
            runJs(click, [this, attempt](const QVariant &r) {
                logger::event(QStringLiteral("publish.confirm"), r.toString());
                QTimer::singleShot(2000, this, [this, attempt]() { verifyPublish(attempt + 1); });
            });
            return;
        }

        const QString dump = dumpPage();
        logger::event(QStringLiteral("publish.not-confirmed"), dump);
        setState(QStringLiteral("error"),
                 QStringLiteral("帖子未确认发出（编辑器仍打开），页面已导出：%1").arg(dump));
        emit failed(m_message);
    });
}

void ChannelPublisher::stepCollectLinks()
{
    setState(QStringLiteral("collecting"), QStringLiteral("正在读取图片直链…"));

    const QString script = QStringLiteral(R"JS(
(function () {
    var imgs = Array.from(document.querySelectorAll(".preview-list img"));
    var urls = imgs.map(function (e) { return e.currentSrc || e.src || ""; })
        .filter(function (u) { return u && u.indexOf("photo.store.qq.com") >= 0; });
    return JSON.stringify(urls);
})()
)JS");

    runJs(script, [this](const QVariant &value) {
        QStringList remote;
        for (const QJsonValue &item : QJsonDocument::fromJson(value.toString().toUtf8()).array()) {
            const QString url = item.toString();
            if (!url.isEmpty())
                remote.append(url);
        }
        if (remote.size() < m_files.size() && ++m_pollTicks < 12) {
            QTimer::singleShot(kPollIntervalMs, this, &ChannelPublisher::pollPreview);
            return;
        }
        if (remote.isEmpty()) {
            const QString path = dumpPage();
            setState(QStringLiteral("error"),
                     QStringLiteral("没有取到图片直链，页面已导出：%1").arg(path));
            emit failed(m_message);
            return;
        }
        logger::event(QStringLiteral("publish.links"), QStringLiteral("count=%1").arg(remote.size()));
        for (const QString &url : remote)
            logger::event(QStringLiteral("publish.link"), url.left(160));
        setState(QStringLiteral("done"), QStringLiteral("已取到 %1 个图片直链").arg(remote.size()));
        emit finished(remote);
    });
}

QString ChannelPublisher::dumpPage()
{
    const QString path = QDir(cacheDir()).filePath(
        QStringLiteral("channel-%1.html")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    m_page->toHtml([path](const QString &html) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(html.toUtf8());
    });
    return path;
}

} // namespace cj
