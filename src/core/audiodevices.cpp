#include "core/audiodevices.h"

#include <QMediaDevices>

namespace audio {

QList<QAudioDevice> inputs()
{
    return QMediaDevices::audioInputs();
}

QList<QAudioDevice> outputs()
{
    return QMediaDevices::audioOutputs();
}

QString deviceId(const QAudioDevice &device)
{
    return device.isNull() ? QString() : QString::fromLatin1(device.id().toHex());
}

QString displayName(const QAudioDevice &device)
{
    return device.isNull() ? QStringLiteral("系统默认") : device.description();
}

QAudioDevice findInput(const QString &id)
{
    if (!id.isEmpty()) {
        for (const QAudioDevice &device : inputs()) {
            if (deviceId(device) == id)
                return device;
        }
    }
    return QMediaDevices::defaultAudioInput();
}

QAudioDevice findOutput(const QString &id)
{
    if (!id.isEmpty()) {
        for (const QAudioDevice &device : outputs()) {
            if (deviceId(device) == id)
                return device;
        }
    }
    return QMediaDevices::defaultAudioOutput();
}

} // namespace audio
