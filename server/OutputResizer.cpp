// SPDX-FileCopyrightText: 2026 KRdp contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "OutputResizer.h"

#include <cmath>
#include <memory>

#include <QEventLoop>
#include <QLoggingCategory>
#include <QPointer>
#include <QRect>

#include <KScreen/Config>
#include <KScreen/GetConfigOperation>
#include <KScreen/Mode>
#include <KScreen/Output>
#include <KScreen/SetConfigOperation>

Q_LOGGING_CATEGORY(KRDP, "org.kde.krdp")

using namespace std::chrono_literals;

// Clients send a layout for every step of a window drag. The first one is applied right away,
// the ones following within this time are merged into one.
constexpr auto SettleDelay = 60ms;
// The smallest desktop size [MS-RDPEDISP] allows a client to ask for.
constexpr int MinimumWidth = 200;
constexpr int MinimumHeight = 200;

static KScreen::OutputPtr findStreamedOutput(const KScreen::ConfigPtr &config, const QSize &streamSize)
{
    KScreen::OutputPtr best;
    const auto outputs = config->outputs();
    for (const auto &output : outputs) {
        if (!output->isConnected() || !output->isEnabled() || !output->currentMode()) {
            continue;
        }
        if (streamSize.isValid() && output->geometry().size() != streamSize) {
            continue;
        }
        if (!best || (output->priority() != 0 && output->priority() < best->priority())) {
            best = output;
        }
    }
    return best;
}

static KScreen::ModePtr nearestMode(const KScreen::OutputPtr &output, const QSize &wanted)
{
    const auto current = output->currentMode();
    const double currentRate = current ? current->refreshRate() : 60.0;

    KScreen::ModePtr best;
    double bestCost = 0;
    const auto modes = output->modes();
    for (const auto &mode : modes) {
        const QSize size = mode->size();
        // distance in log space, so that 2x too big and 2x too small cost the same
        const double cost = std::abs(std::log(double(size.width()) / wanted.width()))
            + std::abs(std::log(double(size.height()) / wanted.height()))
            // among equal sizes prefer the refresh rate we are already running
            + std::abs(mode->refreshRate() - currentRate) * 1e-4;
        if (!best || cost < bestCost) {
            best = mode;
            bestCost = cost;
        }
    }
    return best;
}

/**
 * Timings for a custom mode of exactly @p size, following CVT with reduced blanking.
 *
 * Without explicit timings KWin generates them itself, but then makes the width a multiple
 * of 8, so the desktop would end up narrower than the client's window.
 */
static KScreen::Cvt reducedBlankingTimings(const QSize &size, float refreshRate)
{
    constexpr double MinimumVerticalBlankMicroseconds = 460.0;
    constexpr int VerticalFrontPorch = 3;
    constexpr int VerticalSyncWidth = 10; // the value CVT uses for aspect ratios it has no entry for
    constexpr int MinimumVerticalBackPorch = 6;

    KScreen::Cvt timings{};
    timings.hdisplay = size.width();
    timings.hsyncStart = size.width() + 48;
    timings.hsyncEnd = size.width() + 80;
    timings.htotal = size.width() + 160;

    const double lineMicroseconds = (1e6 / refreshRate - MinimumVerticalBlankMicroseconds) / size.height();
    const int blankLines =
        std::max(int(std::ceil(MinimumVerticalBlankMicroseconds / lineMicroseconds)), VerticalFrontPorch + VerticalSyncWidth + MinimumVerticalBackPorch);
    timings.vdisplay = size.height();
    timings.vsyncStart = size.height() + VerticalFrontPorch;
    timings.vsyncEnd = size.height() + VerticalFrontPorch + VerticalSyncWidth;
    timings.vtotal = size.height() + blankLines;

    // pixel clock in kHz, in the 250 kHz steps CVT uses
    timings.clock = uint32_t(std::floor(refreshRate * timings.htotal * timings.vtotal / 1000.0 / 250.0) * 250);
    timings.flags = 0x1 | 0x8; // DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC, as CVT-RB has
    return timings;
}

/**
 * The size to ask for: the requested one, but within what the output already offers, as we
 * cannot know the limits of the hardware.
 */
static QSize clampToOutput(const KScreen::OutputPtr &output, const QSize &wanted)
{
    qint64 maxArea = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    const auto modes = output->modes();
    for (const auto &mode : modes) {
        const QSize size = mode->size();
        maxArea = std::max<qint64>(maxArea, qint64(size.width()) * size.height());
        maxWidth = std::max(maxWidth, size.width());
        maxHeight = std::max(maxHeight, size.height());
    }
    // Clamp each dimension on its own instead of scaling to keep the aspect ratio: clients don't
    // scale the desktop up, so the biggest desktop the hardware can do leaves the smallest borders.
    QSize size = wanted.boundedTo(QSize(maxWidth, maxHeight));
    if (qint64(size.width()) * size.height() > maxArea) {
        size.setHeight(int(maxArea / size.width()));
    }
    // [MS-RDPEDISP] requires an even width, keep to that also when we had to make it smaller
    size.setWidth(size.width() & ~1);
    return size;
}

static KScreen::ModePtr exactMode(const KScreen::OutputPtr &output, const QSize &size, float preferredRate)
{
    KScreen::ModePtr best;
    const auto modes = output->modes();
    for (const auto &mode : modes) {
        if (mode->size() == size && (!best || std::abs(mode->refreshRate() - preferredRate) < std::abs(best->refreshRate() - preferredRate))) {
            best = mode;
        }
    }
    return best;
}

OutputResizer::OutputResizer(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(SettleDelay);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        if (m_dirty && !m_busy) {
            startApply();
        }
    });
}

OutputResizer::~OutputResizer()
{
    if (m_original) {
        restore(true);
    }
}

void OutputResizer::requestSize(const void *owner, const QSize &size, const QSize &streamSize)
{
    if (size.width() < MinimumWidth || size.height() < MinimumHeight) {
        qCDebug(KRDP) << "Ignoring requested desktop size" << size;
        return;
    }
    m_owners.insert(owner);
    m_pendingSize = size;
    m_dirty = true;
    // keep the first stream size: once we changed the mode, the stream is no longer that size
    if (!m_original) {
        m_streamSize = streamSize;
    }
    // Apply right away unless something is in flight or just finished, then it is picked up
    // when that is done.
    if (!m_busy && !m_timer.isActive()) {
        startApply();
    }
}

void OutputResizer::release(const void *owner)
{
    m_owners.remove(owner);
    if (m_owners.isEmpty()) {
        m_timer.stop();
        m_dirty = false;
        if (m_original) {
            restore(false);
        }
    }
}

void OutputResizer::startApply()
{
    m_dirty = false;
    m_busy = true;
    m_timer.start();

    // Released once the last step of the (asynchronous, several steps long) change is done,
    // whichever way it ends.
    QPointer<OutputResizer> self(this);
    const std::shared_ptr<void> done(nullptr, [self](void *) {
        if (self) {
            self->applyFinished();
        }
    });
    apply(m_pendingSize, done);
}

void OutputResizer::applyFinished()
{
    m_busy = false;
    if (m_dirty && !m_timer.isActive()) {
        startApply();
    } else if (!m_dirty) {
        Q_EMIT idle();
    }
}

void OutputResizer::apply(const QSize &wanted, const std::shared_ptr<void> &done)
{
    qCDebug(KRDP) << "applying size" << wanted;
    auto op = new KScreen::GetConfigOperation(KScreen::GetConfigOperation::NoEDID, this);
    connect(op, &KScreen::GetConfigOperation::finished, this, [this, wanted, done](KScreen::ConfigOperation *op) {
        op->deleteLater();
        if (op->hasError() || m_owners.isEmpty()) {
            return;
        }

        const auto config = op->config();
        KScreen::OutputPtr output;
        if (m_original) {
            const auto outputs = config->outputs();
            for (const auto &candidate : outputs) {
                if (candidate->name() == m_original->outputName) {
                    output = candidate;
                }
            }
        } else {
            output = findStreamedOutput(config, m_streamSize);
        }
        if (!output) {
            qCWarning(KRDP) << "Could not find the streamed output to resize";
            return;
        }

        const QSize target = clampToOutput(output, wanted);
        // Not the current mode's rate: the rate of a custom mode comes out a little lower than
        // asked for, so that would make each new one slower than the last.
        const float rate = std::round(m_original ? m_original->refreshRate : output->currentMode()->refreshRate());
        if (const auto mode = exactMode(output, target, rate)) {
            switchMode(config, output->name(), mode->id(), wanted, done);
            return;
        }

        // No such mode: create one of exactly the size the client asked for, so that the
        // desktop fits the client's window instead of being scrolled or scaled. Clients
        // follow the size they asked for, not the one they get, so it has to be exact.
        qCInfo(KRDP) << "Adding a custom mode of" << target << "on" << output->name();
        auto withMode = config->clone();
        const auto outputs = withMode->outputs();
        for (const auto &candidate : outputs) {
            if (candidate->name() != output->name()) {
                continue;
            }
            auto customModes = candidate->customModes();
            KScreen::ModeInfo info;
            info.size = target;
            info.refreshRate = rate;
            info.flags.setFlag(KScreen::ModeInfo::Flag::Custom);
            info.flags.setFlag(KScreen::ModeInfo::Flag::ReducedBlanking);
            info.cvt = reducedBlankingTimings(target, rate);
            customModes.append(info);
            candidate->setCustomModes(customModes);
        }
        const QString outputName = output->name();
        const Original original{outputName, output->currentMode()->size(), output->currentMode()->refreshRate()};
        auto add = new KScreen::SetConfigOperation(withMode, this);
        connect(add, &KScreen::SetConfigOperation::finished, this, [this, add, outputName, target, wanted, original, rate, done](KScreen::ConfigOperation *) {
            qCDebug(KRDP) << "custom mode added";
            add->deleteLater();
            auto get = new KScreen::GetConfigOperation(KScreen::GetConfigOperation::NoEDID, this);
            connect(get, &KScreen::GetConfigOperation::finished, this, [this, outputName, target, wanted, original, rate, done](KScreen::ConfigOperation *get) {
                get->deleteLater();
                if (get->hasError() || m_owners.isEmpty()) {
                    return;
                }
                const auto config = get->config();
                const auto outputs = config->outputs();
                for (const auto &candidate : outputs) {
                    if (candidate->name() != outputName) {
                        continue;
                    }
                    if (!m_original) {
                        m_original = original;
                    }
                    m_customSizes.append(target);
                    if (const auto mode = exactMode(candidate, target, rate)) {
                        switchMode(config, outputName, mode->id(), wanted, done);
                    } else {
                        qCWarning(KRDP) << "The custom mode was not created, using the nearest mode";
                        if (const auto nearest = nearestMode(candidate, wanted)) {
                            switchMode(config, outputName, nearest->id(), wanted, done);
                        }
                    }
                    return;
                }
            });
        });
    });
}

void OutputResizer::switchMode(const KScreen::ConfigPtr &config,
                               const QString &outputName,
                               const QString &modeId,
                               const QSize &wanted,
                               const std::shared_ptr<void> &done)
{
    auto newConfig = config->clone();
    const auto outputs = newConfig->outputs();
    bool changed = false;
    QSize newSize;
    for (const auto &output : outputs) {
        if (output->name() != outputName || output->currentModeId() == modeId || !output->modes().contains(modeId)) {
            continue;
        }
        const auto mode = output->modes().value(modeId);
        qCInfo(KRDP) << "Switching" << outputName << "to" << mode->size() << "@" << mode->refreshRate() << "for requested size" << wanted;
        if (!m_original) {
            m_original = Original{outputName, output->currentMode()->size(), output->currentMode()->refreshRate()};
        }
        output->setCurrentModeId(modeId);
        newSize = mode->size();
        // forget custom modes we created earlier, the one in use is kept
        QList<KScreen::ModeInfo> keep;
        const auto customModes = output->customModes();
        for (const auto &info : customModes) {
            if (info.size == mode->size() || !m_customSizes.contains(info.size)) {
                keep.append(info);
            }
        }
        m_customSizes.removeIf([&](const QSize &size) {
            return size != mode->size();
        });
        output->setCustomModes(keep);
        changed = true;
    }
    if (!changed) {
        return;
    }
    auto set = new KScreen::SetConfigOperation(newConfig, this);
    connect(set, &KScreen::SetConfigOperation::finished, this, [this, set, done, outputName, newSize](KScreen::ConfigOperation *) {
        qCDebug(KRDP) << "mode switch applied";
        set->deleteLater();
        if (set->hasError()) {
            qCWarning(KRDP) << "Changing the output mode failed:" << set->errorString();
        } else {
            Q_EMIT modeChanged(outputName, newSize);
        }
    });
}

void OutputResizer::restore(bool blocking)
{
    const Original original = *m_original;
    const QList<QSize> customSizes = m_customSizes;
    m_original.reset();
    m_customSizes.clear();

    QEventLoop loop;
    auto done = [blocking, &loop]() {
        if (blocking) {
            loop.quit();
        }
    };

    // The operations are never used once they have finished, they delete themselves.
    auto get = new KScreen::GetConfigOperation(KScreen::GetConfigOperation::NoEDID);
    connect(get, &KScreen::GetConfigOperation::finished, this, [original, customSizes, done](KScreen::ConfigOperation *op) {
        if (op->hasError()) {
            done();
            return;
        }
        auto newConfig = op->config()->clone();
        bool changed = false;
        const auto outputs = newConfig->outputs();
        for (const auto &output : outputs) {
            const auto mode = output->name() == original.outputName ? exactMode(output, original.size, original.refreshRate) : KScreen::ModePtr();
            if (!mode || mode->id() == output->currentModeId()) {
                continue;
            }
            qCInfo(KRDP) << "Restoring" << output->name() << "to" << mode->size();
            output->setCurrentModeId(mode->id());
            QList<KScreen::ModeInfo> keep;
            const auto customModes = output->customModes();
            for (const auto &info : customModes) {
                if (!customSizes.contains(info.size)) {
                    keep.append(info);
                }
            }
            output->setCustomModes(keep);
            changed = true;
        }
        if (!changed) {
            done();
            return;
        }
        auto set = new KScreen::SetConfigOperation(newConfig);
        connect(set, &KScreen::SetConfigOperation::finished, set, [done](KScreen::ConfigOperation *) {
            done();
        });
    });

    if (blocking) {
        // At shutdown: wait for the mode to be restored, but not forever
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
    }
}
