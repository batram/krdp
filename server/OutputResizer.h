// SPDX-FileCopyrightText: 2026 KRdp contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>
#include <optional>

#include <QList>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>
#include <QTimer>

#include <KScreen/Types>

/**
 * Follows the desktop size requested by an RDP client by switching the video
 * mode of the physical output being streamed, using KScreen.
 *
 * Virtual monitors can be resized through the screencast stream itself; this is
 * for sessions that share a real output. The original mode is restored once the
 * last client that asked for a size has gone.
 */
class OutputResizer : public QObject
{
    Q_OBJECT
public:
    explicit OutputResizer(QObject *parent = nullptr);
    ~OutputResizer() override;

    /**
     * Ask for the streamed output to switch to a mode of @p size, or the nearest it can do.
     *
     * @param owner Identifies the requesting connection, see release().
     * @param size The desktop size the client wants.
     * @param streamSize The current size of the output being streamed, used to find it.
     */
    void requestSize(const void *owner, const QSize &size, const QSize &streamSize);

    /**
     * The connection @p owner has gone. Restores the original mode if nobody else is left.
     */
    void release(const void *owner);

    /**
     * Emitted when a change has been applied and no further one is waiting.
     */
    Q_SIGNAL void idle();

    /**
     * Emitted when the output called @p outputName has been switched to a mode of @p size.
     *
     * The compositor needs a moment more before it actually delivers that size.
     */
    Q_SIGNAL void modeChanged(const QString &outputName, const QSize &size);

private:
    void startApply();
    void applyFinished();
    void apply(const QSize &wanted, const std::shared_ptr<void> &done);
    void switchMode(const KScreen::ConfigPtr &config, const QString &outputName, const QString &modeId, const QSize &wanted, const std::shared_ptr<void> &done);
    void restore(bool blocking);

    QTimer m_timer;
    QSet<const void *> m_owners;
    QSize m_pendingSize;
    QSize m_streamSize;
    bool m_busy = false;
    bool m_dirty = false;
    bool m_restorePending = false;

    struct Original {
        QString outputName;
        // mode ids are not stable across configurations, so remember the mode by its properties
        QSize size;
        float refreshRate = 60.0;
    };
    std::optional<Original> m_original;
    // sizes of the custom modes we added to the output
    QList<QSize> m_customSizes;
};
