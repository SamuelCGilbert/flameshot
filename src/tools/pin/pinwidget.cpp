// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#include "pinwidget.h"
#include "core/qguiappcurrentscreen.h"
#include "utils/confighandler.h"
#include "utils/globalvalues.h"
#include "utils/screenshotsaver.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QMenu>
#include <QPinchGesture>
#include <QScreen>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#if !(defined(Q_OS_MACOS) || defined(Q_OS_WIN))
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#endif

namespace {
constexpr int MARGIN = 7;
constexpr int BLUR_RADIUS = 2 * MARGIN;
constexpr qreal STEP = 0.03;
constexpr qreal MIN_SIZE = 100.0;
// The title tells the KWin script of a pin whether a drag is going on
const QString PIN_TITLE = QStringLiteral("flameshot-pin");
const QString PIN_DRAG_TITLE = QStringLiteral("flameshot-pin-drag %1 %2");
}

PinWidget::PinWidget(const QPixmap& pixmap,
                     const QRect& geometry,
                     QWidget* parent)
  : QWidget(parent)
  , m_pixmap(pixmap)
  , m_layout(new QVBoxLayout(this))
  , m_label(new QLabel())
  , m_shadowEffect(new QGraphicsDropShadowEffect(this))
{
    setWindowIcon(QIcon(GlobalValues::iconPath()));
    setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint |
                   Qt::Dialog);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(PIN_TITLE);
    ConfigHandler conf;
    m_baseColor = conf.uiColor();
    m_hoverColor = conf.contrastUiColor();
    QString closePinnedCaptureShortcut =
      conf.shortcut("TYPE_CLOSE_ACTIVE_PIN_WINDOW");
    m_layout->setContentsMargins(MARGIN, MARGIN, MARGIN, MARGIN);

    m_shadowEffect->setColor(m_baseColor);
    m_shadowEffect->setBlurRadius(BLUR_RADIUS);
    m_shadowEffect->setOffset(0, 0);
    setGraphicsEffect(m_shadowEffect);
    setWindowOpacity(m_opacity);

    m_label->setPixmap(m_pixmap);
    m_layout->addWidget(m_label);

    new QShortcut(
      QKeySequence(closePinnedCaptureShortcut), this, SLOT(close()));
    new QShortcut(Qt::Key_Escape, this, SLOT(close()));

    // The capture geometry was converted to device pixels with the ratio of
    // the screenshot, which travels with the pixmap. It is the right one to
    // convert back, the screen's ratio as seen by this process can differ
    // (e.g. the daemon on Wayland with fractional scaling).
    qreal devicePixelRatio = m_pixmap.devicePixelRatio();
    QScreen* currentScreen = QGuiAppCurrentScreen().currentScreen();
    if (devicePixelRatio <= 0 && currentScreen != nullptr) {
        devicePixelRatio = currentScreen->devicePixelRatio();
    }

    const int margin =
      static_cast<int>(static_cast<double>(MARGIN) * devicePixelRatio);
    QRect adjusted_pos = geometry + QMargins(margin, margin, margin, margin);
    setGeometry(adjusted_pos);

    if (currentScreen != nullptr) {
        QPoint topLeft = currentScreen->geometry().topLeft();
        adjusted_pos.setX((adjusted_pos.x() - topLeft.x()) / devicePixelRatio +
                          topLeft.x());

        adjusted_pos.setY((adjusted_pos.y() - topLeft.y()) / devicePixelRatio +
                          topLeft.y());
        adjusted_pos.setWidth(adjusted_pos.size().width() / devicePixelRatio);
        adjusted_pos.setHeight(adjusted_pos.size().height() / devicePixelRatio);
        resize(0, 0);
        move(adjusted_pos.x(), adjusted_pos.y());
    }
    m_initialPosition = pos();

    grabGesture(Qt::PinchGesture);

    this->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this,
            &QWidget::customContextMenuRequested,
            this,
            &PinWidget::showContextMenu);
}

PinWidget::~PinWidget()
{
    unloadKWinScript();
}

void PinWidget::closePin()
{
    update();
    close();
}
bool PinWidget::scrollEvent(QWheelEvent* event)
{
    const auto phase = event->phase();
    if (phase == Qt::ScrollPhase::ScrollUpdate
#if defined(Q_OS_UNIX) || defined(Q_OS_WINDOWS)
        || phase == Qt::ScrollPhase::NoScrollPhase
#endif
    ) {
        const auto angle = event->angleDelta();
        if (angle.y() == 0) {
            return true;
        }
        m_currentStepScaleFactor = angle.y() > 0
                                     ? m_currentStepScaleFactor + STEP
                                     : m_currentStepScaleFactor - STEP;
        m_expanding = m_currentStepScaleFactor >= 1.0;
    }
#if defined(Q_OS_MACOS)
    // ScrollEnd is currently supported only on Mac OSX
    if (phase == Qt::ScrollPhase::ScrollEnd) {
#else
    else {
#endif
        m_scaleFactor *= m_currentStepScaleFactor;
        m_currentStepScaleFactor = 1.0;
        m_expanding = false;
    }

    m_sizeChanged = true;
    update();
    return true;
}

void PinWidget::enterEvent(QEnterEvent*)
{
    m_shadowEffect->setColor(m_hoverColor);
}

void PinWidget::leaveEvent(QEvent*)
{
    m_shadowEffect->setColor(m_baseColor);
}

void PinWidget::mousePressEvent(QMouseEvent* e)
{
    // Don't hand the pointer to the window manager right away: on Wayland a
    // system move grabs the pointer until the button is released, which
    // swallows clicks and double clicks. Start it once the mouse moves.
    if (e->button() == Qt::LeftButton) {
        m_movePending = true;
        m_pressPosition = e->position();
    }
}

void PinWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_movePending || !(e->buttons() & Qt::LeftButton)) {
        return;
    }
    const QPointF distance = e->position() - m_pressPosition;
    if (distance.manhattanLength() < QApplication::startDragDistance()) {
        return;
    }
    m_movePending = false;
    if (!m_kwinScriptName.isEmpty()) {
        // A system move on KWin always keeps 100 pixels of the window on
        // screen, the script has no such limit and can park a pin off screen.
        m_kwinMoving = true;
        setWindowTitle(PIN_DRAG_TITLE.arg(qRound(m_pressPosition.x()))
                         .arg(qRound(m_pressPosition.y())));
        return;
    }
    if (QWindow* window = windowHandle(); window != nullptr) {
        window->startSystemMove();
    }
}

void PinWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        m_movePending = false;
        if (m_kwinMoving) {
            m_kwinMoving = false;
            setWindowTitle(PIN_TITLE);
        }
    }
}

void PinWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton &&
        ConfigHandler().closePinOnDoubleClick()) {
        closePin();
    }
}

void PinWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!m_initialPlacementDone) {
        m_initialPlacementDone = true;
        if (QGuiApplication::platformName() == QLatin1String("wayland")) {
            // Wait until the layout gave the widget its final size
            QTimer::singleShot(0, this, &PinWidget::placeWithKWin);
        }
    }
}

/**
 * @brief Manage the pin's window through KWin on KDE Plasma (Wayland).
 *
 * Wayland clients can't position their own windows or keep them above the
 * others, so the pin would end up wherever the compositor puts new windows.
 * KWin can do it through its scripting API, so a short script is loaded over
 * D-Bus for as long as the pin exists. It moves the new pin window to where
 * the capture was taken and keeps it above.
 * Placed pins are marked by keepAbove, which tells new pins apart from the
 * existing ones of this process.
 *
 * The script also drags the pin, because KWin keeps 100 pixels of a window on
 * screen in a system move and pulls it back to that limit on the next commit
 * (e.g. when it loses focus), which makes it impossible to park a pin mostly
 * off screen. Moves made by a script aren't limited, so the script follows the
 * cursor during a drag and undoes KWin pulling a parked pin back. A script
 * can't be called from outside, so the pin starts and stops the drag through
 * its window title.
 */
void PinWidget::placeWithKWin()
{
#if !(defined(Q_OS_MACOS) || defined(Q_OS_WIN))
    static int counter = 0;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || bus.interface() == nullptr ||
        !bus.interface()->isServiceRegistered(QStringLiteral("org.kde.KWin"))) {
        return;
    }

    const QString pluginName = QStringLiteral("flameshot-pin-%1-%2")
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(++counter);
    QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (dir.isEmpty()) {
        dir = QDir::tempPath();
    }
    const QString path = QDir(dir).filePath(pluginName + QStringLiteral(".js"));

    const QString script = QStringLiteral(R"((function () {
    const PID = %1;
    let X = %2, Y = %3, W = %4, H = %5;
    let pin = null;
    // Where the pin is held during a drag, relative to its top left corner
    let grab = null;
    function matches(w) {
        const g = w.frameGeometry;
        return !pin && w.pid === PID && w.caption === "flameshot-pin" &&
               !w.keepAbove && Math.abs(g.width - W) < 2 &&
               Math.abs(g.height - H) < 2;
    }
    function moveTo(x, y) {
        const g = pin.frameGeometry;
        pin.frameGeometry = { x: x, y: y, width: g.width, height: g.height };
    }
    // Partly outside the screens, but not lost on a screen that went away
    function isParked() {
        let onScreen = false;
        for (const screen of workspace.screens) {
            const s = screen.geometry;
            const left = Math.max(X, s.x), right = Math.min(X + W, s.x + s.width);
            const top = Math.max(Y, s.y), bottom = Math.min(Y + H, s.y + s.height);
            if (left < right && top < bottom) {
                if (right - left >= W && bottom - top >= H) {
                    return false;
                }
                onScreen = true;
            }
        }
        return onScreen;
    }
    function onGeometryChanged() {
        const g = pin.frameGeometry;
        W = g.width;
        H = g.height;
        if (grab || (Math.abs(g.x - X) < 1 && Math.abs(g.y - Y) < 1)) {
            return;
        }
        if (!pin.move && isParked()) {
            moveTo(X, Y);
        } else {
            X = g.x;
            Y = g.y;
        }
    }
    function follow() {
        if (pin && grab) {
            const cursor = workspace.cursorPos;
            X = cursor.x - grab.x;
            Y = cursor.y - grab.y;
            moveTo(X, Y);
        }
    }
    function onCaptionChanged() {
        const drag = /^flameshot-pin-drag (\d+) (\d+)/.exec(pin.caption);
        if (drag) {
            grab = { x: Number(drag[1]), y: Number(drag[2]) };
            follow();
        } else if (grab) {
            grab = null;
            onGeometryChanged();
        }
    }
    function place(w) {
        pin = w;
        moveTo(X, Y);
        w.keepAbove = true;
        // KWin resets keepAbove right after a new window is set up, so set it
        // once more.
        function keepAbove() {
            if (!w.keepAbove) {
                w.keepAboveChanged.disconnect(keepAbove);
                w.keepAbove = true;
            }
        }
        w.keepAboveChanged.connect(keepAbove);
        w.frameGeometryChanged.connect(onGeometryChanged);
        w.captionChanged.connect(onCaptionChanged);
        // Hiding the pin (e.g. while saving it) destroys its window, the next
        // one is placed where this one was.
        w.closed.connect(function () {
            pin = null;
            grab = null;
        });
    }
    function onAdded(w) {
        if (matches(w)) {
            place(w);
        }
    }
    workspace.windowAdded.connect(onAdded);
    workspace.cursorPosChanged.connect(follow);
    const windows = workspace.stackingOrder;
    for (let i = windows.length - 1; i >= 0; --i) {
        if (matches(windows[i])) {
            place(windows[i]);
            return;
        }
    }
})();
)")
                             .arg(QCoreApplication::applicationPid())
                             .arg(m_initialPosition.x())
                             .arg(m_initialPosition.y())
                             .arg(width())
                             .arg(height());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(script.toUtf8());
    file.close();

    QDBusMessage load =
      QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
                                     QStringLiteral("/Scripting"),
                                     QStringLiteral("org.kde.kwin.Scripting"),
                                     QStringLiteral("loadScript"));
    load << path << pluginName;
    QDBusReply<int> id = bus.call(load);
    if (id.isValid() && id.value() >= 0) {
        bus.call(QDBusMessage::createMethodCall(
          QStringLiteral("org.kde.KWin"),
          QStringLiteral("/Scripting/Script%1").arg(id.value()),
          QStringLiteral("org.kde.kwin.Script"),
          QStringLiteral("run")));
        m_kwinScriptName = pluginName;
        connect(qApp,
                &QCoreApplication::aboutToQuit,
                this,
                &PinWidget::unloadKWinScript);
    }

    // KWin reads the file in the background after the call to run
    QTimer::singleShot(5000, [path]() { QFile::remove(path); });
#endif
}

void PinWidget::unloadKWinScript()
{
#if !(defined(Q_OS_MACOS) || defined(Q_OS_WIN))
    if (m_kwinScriptName.isEmpty()) {
        return;
    }
    QDBusMessage unload =
      QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
                                     QStringLiteral("/Scripting"),
                                     QStringLiteral("org.kde.kwin.Scripting"),
                                     QStringLiteral("unloadScript"));
    unload << m_kwinScriptName;
    QDBusConnection::sessionBus().call(unload);
    m_kwinScriptName.clear();
#endif
}

void PinWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_0) {
        m_opacity = 1.0;
    } else if (event->key() == Qt::Key_9) {
        m_opacity = 0.9;
    } else if (event->key() == Qt::Key_8) {
        m_opacity = 0.8;
    } else if (event->key() == Qt::Key_7) {
        m_opacity = 0.7;
    } else if (event->key() == Qt::Key_6) {
        m_opacity = 0.6;
    } else if (event->key() == Qt::Key_5) {
        m_opacity = 0.5;
    } else if (event->key() == Qt::Key_4) {
        m_opacity = 0.4;
    } else if (event->key() == Qt::Key_3) {
        m_opacity = 0.3;
    } else if (event->key() == Qt::Key_2) {
        m_opacity = 0.2;
    } else if (event->key() == Qt::Key_1) {
        m_opacity = 0.1;
    }

    setWindowOpacity(m_opacity);
}
bool PinWidget::gestureEvent(QGestureEvent* event)
{
    if (QGesture* pinch = event->gesture(Qt::PinchGesture)) {
        pinchTriggered(static_cast<QPinchGesture*>(pinch));
    }
    return true;
}

void PinWidget::rotateLeft()
{
    m_sizeChanged = true;

    auto rotateTransform = QTransform().rotate(270);
    m_pixmap = m_pixmap.transformed(rotateTransform);
}

void PinWidget::rotateRight()
{
    m_sizeChanged = true;

    auto rotateTransform = QTransform().rotate(90);
    m_pixmap = m_pixmap.transformed(rotateTransform);
}

void PinWidget::increaseOpacity()
{
    m_opacity += 0.1;
    if (m_opacity > 1.0) {
        m_opacity = 1.0;
    }
    setWindowOpacity(m_opacity);
}

void PinWidget::decreaseOpacity()
{
    m_opacity -= 0.1;
    if (m_opacity < 0.0) {
        m_opacity = 0.0;
    }

    setWindowOpacity(m_opacity);
}

bool PinWidget::event(QEvent* event)
{
    if (event->type() == QEvent::Gesture) {
        return gestureEvent(static_cast<QGestureEvent*>(event));
    } else if (event->type() == QEvent::Wheel) {
        return scrollEvent(static_cast<QWheelEvent*>(event));
    }
    return QWidget::event(event);
}

void PinWidget::paintEvent(QPaintEvent* event)
{
    if (m_sizeChanged) {
        const auto aspectRatio =
          m_expanding ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio;
        const auto transformType = ConfigHandler().antialiasingPinZoom()
                                     ? Qt::SmoothTransformation
                                     : Qt::FastTransformation;
        const qreal iw = m_pixmap.width();
        const qreal ih = m_pixmap.height();
        const qreal nw = qBound(MIN_SIZE,
                                iw * m_currentStepScaleFactor * m_scaleFactor,
                                static_cast<qreal>(maximumWidth()));
        const qreal nh = qBound(MIN_SIZE,
                                ih * m_currentStepScaleFactor * m_scaleFactor,
                                static_cast<qreal>(maximumHeight()));

        const QPixmap pix = m_pixmap.scaled(nw, nh, aspectRatio, transformType);

        m_label->setPixmap(pix);
        adjustSize();
        m_sizeChanged = false;
    }
}

void PinWidget::pinchTriggered(QPinchGesture* gesture)
{
    const QPinchGesture::ChangeFlags changeFlags = gesture->changeFlags();
    if (changeFlags & QPinchGesture::ScaleFactorChanged) {
        m_currentStepScaleFactor = gesture->totalScaleFactor();
        m_expanding = m_currentStepScaleFactor > gesture->lastScaleFactor();
    }
    if (gesture->state() == Qt::GestureFinished) {
        m_scaleFactor *= m_currentStepScaleFactor;
        m_currentStepScaleFactor = 1;
        m_expanding = false;
    }
    m_sizeChanged = true;
    update();
}

void PinWidget::showContextMenu(const QPoint& pos)
{
    QMenu contextMenu(tr("Context menu"), this);

    QAction copyToClipboardAction(tr("Copy to clipboard"), this);
    connect(&copyToClipboardAction,
            &QAction::triggered,
            this,
            &PinWidget::copyToClipboard);
    contextMenu.addAction(&copyToClipboardAction);

    QAction saveToFileAction(tr("Save to file"), this);
    connect(
      &saveToFileAction, &QAction::triggered, this, &PinWidget::saveToFile);
    contextMenu.addAction(&saveToFileAction);

    contextMenu.addSeparator();

    QAction rotateRightAction(tr("Rotate Right"), this);
    connect(
      &rotateRightAction, &QAction::triggered, this, &PinWidget::rotateRight);
    contextMenu.addAction(&rotateRightAction);

    QAction rotateLeftAction(tr("Rotate Left"), this);
    connect(
      &rotateLeftAction, &QAction::triggered, this, &PinWidget::rotateLeft);
    contextMenu.addAction(&rotateLeftAction);

    QAction increaseOpacityAction(tr("Increase Opacity"), this);
    connect(&increaseOpacityAction,
            &QAction::triggered,
            this,
            &PinWidget::increaseOpacity);
    contextMenu.addAction(&increaseOpacityAction);

    QAction decreaseOpacityAction(tr("Decrease Opacity"), this);
    connect(&decreaseOpacityAction,
            &QAction::triggered,
            this,
            &PinWidget::decreaseOpacity);
    contextMenu.addAction(&decreaseOpacityAction);

    QAction closePinAction(tr("Close"), this);
    connect(&closePinAction, &QAction::triggered, this, &PinWidget::closePin);
    contextMenu.addSeparator();
    contextMenu.addAction(&closePinAction);

    contextMenu.exec(mapToGlobal(pos));
}

void PinWidget::copyToClipboard()
{
    saveToClipboard(m_pixmap);
}
void PinWidget::saveToFile()
{
    hide();
    saveToFilesystemGUI(m_pixmap);
    show();
}
