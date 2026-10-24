#include "qq/qrlogin.h"
#include "core/logger.h"
#include "core/securestore.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/crypto.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace cj {

namespace {

// QQ 频道 web 的登录参数（取自频道前端 component-login-dialog 的真实 iframe 地址）：
//   https://xui.ptlogin2.qq.com/cgi-bin/xlogin?appid=1600001587&hide_close_icon=1&daid=823&s_url=<频道页>
constexpr const char *kAppId = "1600001587";
constexpr const char *kDaid = "823";
constexpr const char *kTargetUrl = "https://pd.qq.com/g/pd55235600";

// QNetworkCookieJar::allCookies 是 protected，这里暴露一个只读入口
class PublicCookieJar : public QNetworkCookieJar
{
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    QList<QNetworkCookie> cookies() const { return allCookies(); }
};

QString cacheDir()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString dir = QDir(root).filePath(QStringLiteral("qq"));
    QDir().mkpath(dir);
    return dir;
}

// 诊断日志：只记阶段与返回码，绝不记 cookie / 令牌 / 昵称
void logLine(const QString &text)
{
    QFile file(QDir(cacheDir()).filePath(QStringLiteral("login.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    file.write(QStringLiteral("[%1] %2\n").arg(stamp, text).toUtf8());
}

// QQ 的 hash33：用于把 qrsig 算成 ptqrtoken
QByteArray hash33(const QByteArray &input)
{
    quint32 e = 0;
    for (char ch : input)
        e += (e << 5) + static_cast<unsigned char>(ch);
    return QByteArray::number(e & 0x7FFFFFFF);
}

} // namespace

QqQrLogin::QqQrLogin(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
    m_net->setCookieJar(new PublicCookieJar(this));
}

void QqQrLogin::setState(const QString &state, const QString &message)
{
    const bool stateDirty = m_state != state;
    m_state = state;
    if (m_message != message) {
        m_message = message;
        emit messageChanged();
    }
    if (stateDirty)
        emit stateChanged();
}

void QqQrLogin::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

QByteArray QqQrLogin::cookieValue(const QString &name) const
{
    auto *jar = dynamic_cast<PublicCookieJar *>(m_net->cookieJar());
    if (!jar)
        return {};
    for (const QNetworkCookie &cookie : jar->cookies()) {
        if (QString::fromLatin1(cookie.name()) == name)
            return cookie.value();
    }
    return {};
}

void QqQrLogin::forgetSession()
{
    securestore::remove(QStringLiteral("qq-session"));
    m_loggedIn = false;
    emit loggedInChanged();
    logger::event(QStringLiteral("qq.session-forgotten"));
}

bool QqQrLogin::restoreSession()
{
    const QByteArray raw = securestore::load(QStringLiteral("qq-session"));
    if (raw.isEmpty()) {
        logger::event(QStringLiteral("qq.session-restore"), QStringLiteral("empty"));
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray()) {
        logger::event(QStringLiteral("qq.session-restore"), QStringLiteral("bad-json"));
        return false;
    }

    auto *jar = dynamic_cast<PublicCookieJar *>(m_net->cookieJar());
    if (!jar)
        return false;

    int restored = 0;
    const QJsonArray array = doc.array();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        QNetworkCookie cookie(object.value(QStringLiteral("name")).toString().toLatin1(),
                              object.value(QStringLiteral("value")).toString().toLatin1());
        const QString domain = object.value(QStringLiteral("domain")).toString();
        if (!domain.isEmpty())
            cookie.setDomain(domain);
        const QString path = object.value(QStringLiteral("path")).toString();
        if (!path.isEmpty())
            cookie.setPath(path);
        const qint64 expires = object.value(QStringLiteral("expires")).toInteger();
        if (expires > 0)
            cookie.setExpirationDate(QDateTime::fromSecsSinceEpoch(expires));
        cookie.setSecure(object.value(QStringLiteral("secure")).toBool());
        cookie.setHttpOnly(object.value(QStringLiteral("httpOnly")).toBool());
        jar->insertCookie(cookie);
        ++restored;
    }

    const bool hasAuth = !cookieValue(QStringLiteral("p_skey")).isEmpty()
                         || !cookieValue(QStringLiteral("uin")).isEmpty();
    logger::event(QStringLiteral("qq.session-restore"),
                  QStringLiteral("cookies=%1 auth=%2").arg(restored).arg(hasAuth ? 1 : 0));
    if (hasAuth) {
        m_loggedIn = true;
        emit loggedInChanged();
        setState(QStringLiteral("loggedIn"), QStringLiteral("QQ 频道已登录"));
        return true;
    }
    return false;
}

void QqQrLogin::saveSession() const
{
    auto *jar = dynamic_cast<PublicCookieJar *>(m_net->cookieJar());
    if (!jar)
        return;
    QJsonArray array;
    for (const QNetworkCookie &cookie : jar->cookies()) {
        if (cookie.isSessionCookie() && cookie.name() != "p_skey" && cookie.name() != "uin")
            continue;
        QJsonObject object;
        object.insert(QStringLiteral("name"), QString::fromLatin1(cookie.name()));
        object.insert(QStringLiteral("value"), QString::fromLatin1(cookie.value()));
        object.insert(QStringLiteral("domain"), cookie.domain());
        object.insert(QStringLiteral("path"), cookie.path());
        object.insert(QStringLiteral("expires"), static_cast<qint64>(cookie.expirationDate().toSecsSinceEpoch()));
        object.insert(QStringLiteral("secure"), cookie.isSecure());
        object.insert(QStringLiteral("httpOnly"), cookie.isHttpOnly());
        array.append(object);
    }
    const QByteArray raw = QJsonDocument(array).toJson(QJsonDocument::Compact);
    if (securestore::save(QStringLiteral("qq-session"), raw))
        logger::event(QStringLiteral("qq.session-saved"), QStringLiteral("cookies=%1").arg(array.size()));
}
void QqQrLogin::start()
{
    if (m_busy)
        return;
    m_loggedIn = false;
    emit loggedInChanged();
    m_pollCount = 0;
    fetchLoginSig();
}

void QqQrLogin::refresh()
{
    abort();
    start();
}

void QqQrLogin::abort()
{
    if (m_pollReply) {
        m_pollReply->abort();
        m_pollReply->deleteLater();
        m_pollReply = nullptr;
    }
    setBusy(false);
}

void QqQrLogin::fetchLoginSig()
{
    setBusy(true);
    setState(QStringLiteral("loading"), QStringLiteral("正在获取二维码…"));

    QUrl url(QStringLiteral("https://xui.ptlogin2.qq.com/cgi-bin/xlogin"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("appid"), QString::fromLatin1(kAppId));
    query.addQueryItem(QStringLiteral("daid"), QString::fromLatin1(kDaid));
    query.addQueryItem(QStringLiteral("style"), QStringLiteral("33"));
    query.addQueryItem(QStringLiteral("login_text"), QStringLiteral("登录"));
    query.addQueryItem(QStringLiteral("hide_title_bar"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("hide_close_icon"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("hide_border"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("target"), QStringLiteral("self"));
    query.addQueryItem(QStringLiteral("s_url"), QString::fromLatin1(kTargetUrl));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                     "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"));

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("无法访问 QQ 登录页：%1").arg(reply->errorString()));
            return;
        }
        logLine(QStringLiteral("xlogin ok, login_sig=%1").arg(m_loginSig.isEmpty() ? QStringLiteral("none") : QStringLiteral("yes")));
        m_loginSig = cookieValue(QStringLiteral("pt_login_sig"));
        fetchQrImage();
    });
}

void QqQrLogin::fetchQrImage()
{
    QUrl url(QStringLiteral("https://ssl.ptlogin2.qq.com/ptqrshow"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("appid"), QString::fromLatin1(kAppId));
    query.addQueryItem(QStringLiteral("e"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("l"), QStringLiteral("M"));
    query.addQueryItem(QStringLiteral("s"), QStringLiteral("3"));
    query.addQueryItem(QStringLiteral("d"), QStringLiteral("72"));
    query.addQueryItem(QStringLiteral("v"), QStringLiteral("4"));
    query.addQueryItem(QStringLiteral("t"),
                       QString::number(QDateTime::currentMSecsSinceEpoch() % 100000000));
    query.addQueryItem(QStringLiteral("daid"), QString::fromLatin1(kDaid));
    if (!m_loginSig.isEmpty())
        query.addQueryItem(QStringLiteral("login_sig"), QString::fromLatin1(m_loginSig));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                     "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"));
    request.setRawHeader("Referer", "https://xui.ptlogin2.qq.com/");

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("二维码获取失败：%1").arg(reply->errorString()));
            return;
        }
        const QByteArray png = reply->readAll();
        if (png.isEmpty() || !png.startsWith("\x89PNG")) {
            fail(QStringLiteral("二维码数据异常，请重试"));
            return;
        }
        m_qrsig = cookieValue(QStringLiteral("qrsig"));
        if (m_qrsig.isEmpty()) {
            fail(QStringLiteral("登录会话缺失（qrsig），请重试"));
            return;
        }
        m_ptqrtoken = hash33(m_qrsig);
        logLine(QStringLiteral("ptqrshow ok, bytes=%1, qrsig=%2").arg(png.size()).arg(m_qrsig.isEmpty() ? QStringLiteral("none") : QStringLiteral("yes")));

        const QString path = QDir(cacheDir()).filePath(
            QStringLiteral("qq-qr-%1.png")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HHmmsszzz"))));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(png);
            file.close();
        }
        m_qrImagePath = path;
        emit qrImagePathChanged();

        setState(QStringLiteral("waiting"), QStringLiteral("请用手机 QQ 扫描二维码"));
        startPolling();
    });
}

void QqQrLogin::startPolling()
{
    setBusy(false);
    m_pollCount = 0;
    pollOnce();
}

void QqQrLogin::pollOnce()
{
    if (m_loggedIn)
        return;
    if (m_pollCount++ > 150) {
        setState(QStringLiteral("expired"), QStringLiteral("二维码已失效，请刷新"));
        return;
    }

    QUrl url(QStringLiteral("https://ssl.ptlogin2.qq.com/ptqrlogin"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("u1"), QString::fromLatin1(kTargetUrl));
    query.addQueryItem(QStringLiteral("ptqrtoken"), QString::fromLatin1(m_ptqrtoken));
    query.addQueryItem(QStringLiteral("ptredirect"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("h"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("t"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("g"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("from_ui"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("ptlang"), QStringLiteral("2052"));
    query.addQueryItem(QStringLiteral("action"),
                       QStringLiteral("0-0-%1").arg(QDateTime::currentMSecsSinceEpoch()));
    query.addQueryItem(QStringLiteral("js_ver"), QStringLiteral("10233"));
    query.addQueryItem(QStringLiteral("js_type"), QStringLiteral("1"));
    if (!m_loginSig.isEmpty())
        query.addQueryItem(QStringLiteral("login_sig"), QString::fromLatin1(m_loginSig));
    query.addQueryItem(QStringLiteral("pt_uistyle"), QStringLiteral("40"));
    query.addQueryItem(QStringLiteral("aid"), QString::fromLatin1(kAppId));
    query.addQueryItem(QStringLiteral("daid"), QString::fromLatin1(kDaid));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                     "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"));
    request.setRawHeader("Referer", "https://xui.ptlogin2.qq.com/");

    QNetworkReply *reply = m_net->get(request);
    m_pollReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_pollReply == reply)
            m_pollReply = nullptr;
        reply->deleteLater();

        if (m_loggedIn)
            return;
        if (reply->error() == QNetworkReply::OperationCanceledError)
            return;

        const QString body = QString::fromUtf8(reply->readAll());

        // 响应是 GBK 编码的 ptuiCB('code','0','url','0','msg','')，
        // 只取返回码与跳转地址，不解析中文提示（避免编码问题）
        static const QRegularExpression codePattern(QStringLiteral("ptuiCB\\('(\\d+)'"));
        static const QRegularExpression redirectPattern(
            QStringLiteral("ptuiCB\\('0','\\d+','([^']*)'"));
        const auto codeMatch = codePattern.match(body);
        if (!codeMatch.hasMatch()) {
            logLine(QStringLiteral("ptqrlogin unparsable, bytes=%1, body=%2")
                        .arg(body.size())
                        .arg(QString(body).left(200)));
            QTimer::singleShot(1500, this, &QqQrLogin::pollOnce);
            return;
        }

        const QString code = codeMatch.captured(1);
        QString redirect;
        if (const auto redirectMatch = redirectPattern.match(body); redirectMatch.hasMatch())
            redirect = redirectMatch.captured(1);
        logLine(QStringLiteral("ptqrlogin code=%1").arg(code));

        if (code == QStringLiteral("0")) {
            setState(QStringLiteral("confirmed"), QStringLiteral("已确认，正在完成登录…"));
            finishLogin(redirect);
            return;
        }
        if (code == QStringLiteral("67")) {
            setState(QStringLiteral("scanned"), QStringLiteral("已扫描，请在手机上点击确认"));
        } else if (code == QStringLiteral("66")) {
            setState(QStringLiteral("waiting"), QStringLiteral("请用手机 QQ 扫描二维码"));
        } else if (code == QStringLiteral("65")) {
            setState(QStringLiteral("expired"), QStringLiteral("二维码已失效，请刷新"));
            return;
        } else {
            setState(QStringLiteral("waiting"), QStringLiteral("登录状态：%1，请重试").arg(code));
        }
        QTimer::singleShot(1200, this, &QqQrLogin::pollOnce);
    });
}

void QqQrLogin::finishLogin(const QString &checkSigUrl)
{
    if (checkSigUrl.isEmpty()) {
        fail(QStringLiteral("登录跳转地址缺失"));
        return;
    }

    QNetworkRequest request{QUrl(checkSigUrl)};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                     "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"));

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        // 校验跳转后应拿到 p_skey / skey 一类的会话 cookie
        const bool hasSession = !cookieValue(QStringLiteral("p_skey")).isEmpty()
                                || !cookieValue(QStringLiteral("skey")).isEmpty()
                                || !cookieValue(QStringLiteral("uin")).isEmpty();
        if (!hasSession) {
            fail(QStringLiteral("登录未完成：未取得会话凭据"));
            return;
        }

        // 频道会话要落在 pd.qq.com 域：再走一次频道页，让 cookie 跟着下种
        setState(QStringLiteral("confirmed"), QStringLiteral("正在建立频道会话…"));
        QNetworkRequest channelRequest{QUrl(QString::fromLatin1(kTargetUrl))};
        channelRequest.setHeader(QNetworkRequest::UserAgentHeader,
                                 QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                                "AppleWebKit/537.36 (KHTML, like Gecko) "
                                                "Chrome/131.0.0.0 Safari/537.36"));
        QNetworkReply *channelReply = m_net->get(channelRequest);
        connect(channelReply, &QNetworkReply::finished, this, [this, channelReply]() {
            channelReply->deleteLater();
            m_loggedIn = true;
            saveSession();   // 关键：把会话用 DPAPI 加密落盘，重启后免扫码
            emit loggedInChanged();
            setState(QStringLiteral("loggedIn"), QStringLiteral("QQ 频道已登录"));
            emit loginSucceeded();
        });
    });
}

void QqQrLogin::fail(const QString &message)
{
    setBusy(false);
    setState(QStringLiteral("error"), message);
}

QList<QNetworkCookie> QqQrLogin::sessionCookies() const
{
    if (auto *jar = dynamic_cast<PublicCookieJar *>(m_net->cookieJar()))
        return jar->cookies();
    return {};
}

QString QqQrLogin::cookieNames() const
{
    QStringList names;
    if (auto *jar = dynamic_cast<PublicCookieJar *>(m_net->cookieJar())) {
        for (const QNetworkCookie &cookie : jar->cookies())
            names << QString::fromLatin1(cookie.name());
    }
    names.removeDuplicates();
    return names.join(QStringLiteral(", "));
}

} // namespace cj
