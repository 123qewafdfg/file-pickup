#include "circularreveal.h"

#include <QPainter>
#include <QRadialGradient>

namespace cj {

CircularReveal::CircularReveal(QQuickItem *parent)
    : QQuickPaintedItem(parent)
    , m_animation(new QPropertyAnimation(this, "radius", this))
{
    m_animation->setDuration(m_duration);
    m_animation->setEasingCurve(QEasingCurve::InOutCubic);
    setVisible(false);

    connect(m_animation, &QPropertyAnimation::finished, this, [this]() {
        update();
        setVisible(false);
        emit animationFinished();
    });
}

void CircularReveal::setTarget(QQuickItem *value)
{
    if (m_target == value)
        return;
    m_target = value;
    emit targetChanged();
}

void CircularReveal::setRadius(int value)
{
    if (m_radius == value)
        return;
    m_radius = value;
    emit radiusChanged();
    update();
}

void CircularReveal::setDarkToLight(bool value)
{
    if (m_darkToLight == value)
        return;
    m_darkToLight = value;
    emit darkToLightChanged();
}

void CircularReveal::setDuration(int value)
{
    value = qMax(0, value);
    if (m_duration == value)
        return;
    m_duration = value;
    m_animation->setDuration(m_duration);
    emit durationChanged();
}

void CircularReveal::paint(QPainter *painter)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->drawImage(QRectF(0, 0, width(), height()), m_source);

    // Keep the old-theme snapshot with a feathered radial alpha mask. The
    // 48 px band removes the visibly hard circle edge while the new theme is
    // revealed underneath it.
    constexpr qreal feather = 48.0;
    const qreal outerRadius = qMax<qreal>(1.0, m_radius + feather);
    const qreal innerStop = qBound<qreal>(0.0,
                                          (m_radius - feather) / outerRadius,
                                          1.0);
    QRadialGradient mask(QPointF(m_center), outerRadius);

    if (m_darkToLight) {
        // The new dark theme expands from the button: old snapshot remains
        // outside the circle and fades towards the revealed centre.
        mask.setColorAt(0.0, Qt::transparent);
        mask.setColorAt(innerStop, Qt::transparent);
        mask.setColorAt(1.0, Qt::white);
    } else {
        // The new light theme expands in the opposite direction: old snapshot
        // remains inside the shrinking circle and fades at its outer edge.
        mask.setColorAt(0.0, Qt::white);
        mask.setColorAt(innerStop, Qt::white);
        mask.setColorAt(1.0, Qt::transparent);
    }

    painter->setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter->fillRect(QRectF(0, 0, width(), height()), mask);
    painter->restore();
}

void CircularReveal::start(int imageWidth, int imageHeight,
                           const QPoint &center, int finalRadius,
                           const QRect &sourceRect)
{
    if (!m_target || imageWidth <= 0 || imageHeight <= 0 || finalRadius <= 0
        || !sourceRect.isValid())
        return;

    if (m_animation->state() == QAbstractAnimation::Running)
        m_animation->stop();

    m_center = center;
    m_sourceRect = sourceRect;
    m_captureSize = QSize(imageWidth, imageHeight);
    m_animation->setDuration(m_duration);
    m_animation->setStartValue(m_darkToLight ? finalRadius : 0);
    m_animation->setEndValue(m_darkToLight ? 0 : finalRadius);

    m_grabResult = m_target->grabToImage(QSize(imageWidth, imageHeight));
    if (!m_grabResult)
        return;
    connect(m_grabResult.data(), &QQuickItemGrabResult::ready,
            this, &CircularReveal::handleGrabResult, Qt::SingleShotConnection);
}

void CircularReveal::handleGrabResult()
{
    if (!m_grabResult)
        return;
    const QImage image = m_grabResult->image();
    const qreal scaleX = image.width() / qreal(m_captureSize.width());
    const qreal scaleY = image.height() / qreal(m_captureSize.height());
    const QRect pixelRect(qRound(m_sourceRect.x() * scaleX),
                          qRound(m_sourceRect.y() * scaleY),
                          qRound(m_sourceRect.width() * scaleX),
                          qRound(m_sourceRect.height() * scaleY));
    m_source = image.copy(pixelRect.intersected(image.rect()));
    update();
    setVisible(true);
    emit imageChanged();
    m_animation->start();
}

} // namespace cj
