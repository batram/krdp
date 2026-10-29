// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Clipboard.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/utils/cliprdr_utils.h>
#include <winpr/shell.h>

#include "PeerContext_p.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

using namespace Qt::StringLiterals;

namespace KRdp
{

namespace
{
// The format names Windows uses for copied files, [MS-RDPECLIP] 1.3.1.2
const auto FileGroupDescriptorName = "FileGroupDescriptorW"_L1;
const auto FileContentsName = "FileContents"_L1;

// Ids for the formats we offer under those names; any id from the registered range will do.
constexpr uint32_t ServerFileGroupDescriptorId = 0xC0F0;
constexpr uint32_t ServerFileContentsId = 0xC0F1;

// Size of the pieces file contents are requested in.
constexpr uint32_t FileChunkSize = 1024 * 1024;
// Copied files are fetched as soon as they are copied, don't fetch more than this every time.
constexpr quint64 MaximumIncomingSize = quint64(2) * 1024 * 1024 * 1024;

// The longest name a FILEDESCRIPTORW can hold, without the terminating null
constexpr int MaximumFileNameLength = 259;

// 100 ns intervals between 1601-01-01 and 1970-01-01
constexpr quint64 FileTimeUnixEpoch = 116444736000000000ULL;

quint64 toFileTime(const QDateTime &time)
{
    return FileTimeUnixEpoch + quint64(time.toMSecsSinceEpoch()) * 10000;
}

QString cacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/krdpserver/clipboard"_s;
}

/**
 * A path received from the client, as a relative path below the transfer directory, or an empty
 * string if it is not one that is safe to use.
 */
QString sanitizedRelativePath(const QString &name)
{
    QString path = name;
    path.replace(u'\\', u'/');
    const QStringList parts = path.split(u'/', Qt::SkipEmptyParts);
    if (parts.isEmpty() || path.startsWith(u'/') || path.contains(u':')) {
        return {};
    }
    for (const auto &part : parts) {
        if (part == u".."_s || part == u"."_s) {
            return {};
        }
    }
    return parts.join(u'/');
}
}

class KRDP_NO_EXPORT Clipboard::Private
{
public:
    using CliprdrServerContextPtr = std::unique_ptr<CliprdrServerContext, decltype(&cliprdr_server_context_free)>;

    Private(Clipboard *qq)
        : q(qq)
    {
    }

    Clipboard *q;

    uint32_t onClientCapabilities(const CLIPRDR_CAPABILITIES *capabilities);
    uint32_t onClientFormatList(const CLIPRDR_FORMAT_LIST *formatList);
    uint32_t onClientFormatListResponse(const CLIPRDR_FORMAT_LIST_RESPONSE *formatListResponse);
    uint32_t onClientFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest);
    uint32_t onClientFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse);
    uint32_t onClientFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST *fileContentsRequest);
    uint32_t onClientFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE *fileContentsResponse);

    // Whether both ends can copy files, known once the client sent its capabilities.
    bool fileTransferSupported() const
    {
        return clipContext && clipContext->streamFileClipEnabled;
    }

    void requestFormatData(uint32_t formatId);
    void sendFormatDataResponse(const QByteArray &data, bool ok);

    RdpConnection *session;

    CliprdrServerContextPtr clipContext = CliprdrServerContextPtr(nullptr, cliprdr_server_context_free);

    bool enabled = false;
    // mstsc drops a format list sent before its capabilities response; hold until markClientReady()
    bool clientReady = false;
    void markClientReady()
    {
        if (clientReady) {
            return;
        }
        clientReady = true;
        QMetaObject::invokeMethod(q, &Clipboard::sendServerData, Qt::QueuedConnection);
    }
    std::unique_ptr<const QMimeData> serverData;
    std::unique_ptr<QMimeData> clientData;

    // Only one format data request may be outstanding, and its response does not say which
    // format it is for.
    std::optional<uint32_t> pendingFormatRequest;
    // id the client gave FileGroupDescriptorW in its current format list, or 0
    uint32_t clientFileGroupDescriptorId = 0;

    // Files copied on the client, being fetched into a directory of our own.
    struct IncomingFile {
        QString relativePath;
        bool directory = false;
        std::optional<quint64> size;
    };
    struct IncomingTransfer {
        QString directory;
        QList<IncomingFile> files;
        qsizetype current = -1;
        std::unique_ptr<QFile> file;
        quint64 offset = 0;
        uint32_t streamId = 0;
        bool sizeRequested = false;
        quint64 received = 0;
    };
    std::unique_ptr<IncomingTransfer> incoming;
    uint32_t nextStreamId = 1;

    void startIncomingTransfer(const QByteArray &fileList);
    void fetchNextFile();
    void requestFileContents(uint32_t flags, quint64 position, uint32_t size);
    void abortIncomingTransfer(const QString &reason);
    void finishIncomingTransfer();

    // Files copied on our side, offered to the client.
    struct OutgoingFile {
        QString localPath;
        QString name; // relative, with backslashes
        bool directory = false;
        quint64 size = 0;
        QDateTime lastModified;
    };
    QList<OutgoingFile> outgoingFiles;
    // the file the client is reading
    qsizetype openOutgoingIndex = -1;
    QFile openOutgoingFile;

    void collectOutgoingFiles(const QList<QUrl> &urls);
    QByteArray serializeOutgoingFiles() const;
    void sendFileContentsResponse(uint32_t streamId, const QByteArray &data, bool ok);

    template<typename>
    struct function_arg_trait;
    template<typename Argument>
    struct function_arg_trait<uint32_t (Private::*)(Argument)> {
        typedef Argument argument_t;
    };

    template<auto func>
    inline static UINT processInMainThread(Clipboard *clipboard, function_arg_trait<decltype(func)>::argument_t packet)
    {
        uint32_t channelState;
        QMetaObject::invokeMethod(
            clipboard,
            [clipboard](function_arg_trait<decltype(func)>::argument_t packet) {
                return ((*clipboard->d).*func)(packet);
            },
            Qt::BlockingQueuedConnection,
            qReturnArg(channelState),
            packet);
        return channelState;
    }

    static UINT clientCapabilities(CliprdrServerContext *context, const CLIPRDR_CAPABILITIES *capabilities)
    {
        return processInMainThread<&Private::onClientCapabilities>(reinterpret_cast<Clipboard *>(context->custom), capabilities);
    }

    static UINT clientFormatList(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST *formatList)
    {
        return processInMainThread<&Private::onClientFormatList>(reinterpret_cast<Clipboard *>(context->custom), formatList);
    }

    static UINT clientFormatListResponse(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST_RESPONSE *formatListResponse)
    {
        return processInMainThread<&Private::onClientFormatListResponse>(reinterpret_cast<Clipboard *>(context->custom), formatListResponse);
    }

    static UINT clientFormatDataRequest(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest)
    {
        return processInMainThread<&Private::onClientFormatDataRequest>(reinterpret_cast<Clipboard *>(context->custom), formatDataRequest);
    }

    static UINT clientFormatDataResponse(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse)
    {
        return processInMainThread<&Private::onClientFormatDataResponse>(reinterpret_cast<Clipboard *>(context->custom), formatDataResponse);
    }

    static UINT clientFileContentsRequest(CliprdrServerContext *context, const CLIPRDR_FILE_CONTENTS_REQUEST *fileContentsRequest)
    {
        return processInMainThread<&Private::onClientFileContentsRequest>(reinterpret_cast<Clipboard *>(context->custom), fileContentsRequest);
    }

    static UINT clientFileContentsResponse(CliprdrServerContext *context, const CLIPRDR_FILE_CONTENTS_RESPONSE *fileContentsResponse)
    {
        return processInMainThread<&Private::onClientFileContentsResponse>(reinterpret_cast<Clipboard *>(context->custom), fileContentsResponse);
    }
};

Clipboard::Clipboard(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>(this))
{
    d->session = session;
}

Clipboard::~Clipboard()
{
    if (d->incoming) {
        d->abortIncomingTransfer(u"the connection was closed"_s);
    }
}

bool Clipboard::initialize()
{
    if (d->clipContext) {
        return true;
    }

    auto peerContext = reinterpret_cast<PeerContext *>(d->session->rdpPeer()->context);

    d->clipContext = Private::CliprdrServerContextPtr{cliprdr_server_context_new(peerContext->virtualChannelManager), cliprdr_server_context_free};
    if (!d->clipContext) {
        qCWarning(KRDP) << "Failed creating Clipboard context";
        return false;
    }

    // What we can do; the channel reduces these to what the client can do as well once it
    // knows the client's capabilities.
    d->clipContext->useLongFormatNames = TRUE;
    d->clipContext->streamFileClipEnabled = TRUE;
    d->clipContext->fileClipNoFilePaths = TRUE;
    d->clipContext->canLockClipData = FALSE;
    d->clipContext->hasHugeFileSupport = TRUE;

    d->clipContext->custom = this;
    d->clipContext->rdpcontext = d->session->rdpPeer()->context;

    d->clipContext->ClientCapabilities = Private::clientCapabilities;
    d->clipContext->ClientFormatList = Private::clientFormatList;
    d->clipContext->ClientFormatListResponse = Private::clientFormatListResponse;
    d->clipContext->ClientFormatDataRequest = Private::clientFormatDataRequest;
    d->clipContext->ClientFormatDataResponse = Private::clientFormatDataResponse;
    d->clipContext->ClientFileContentsRequest = Private::clientFileContentsRequest;
    d->clipContext->ClientFileContentsResponse = Private::clientFileContentsResponse;

    // returns 0 on success
    // https://pub.freerdp.com/api/server_2cliprdr__main_8c.html#ab4e8a28c6b4371c2a5f34e8716ab1e9e
    if (d->clipContext->Start(d->clipContext.get())) {
        qCWarning(KRDP) << "Could not start Clipboard context";
        return false;
    };

    d->enabled = true;
    // flush a copy made before the cliprdr channel finished coming up, so it isn't silently lost
    QMetaObject::invokeMethod(this, &Clipboard::sendServerData, Qt::QueuedConnection);

    return true;
}

bool Clipboard::enabled()
{
    return d->enabled;
}

void Clipboard::close()
{
    if (!d->clipContext) {
        return;
    }

    if (d->clipContext->Stop(d->clipContext.get())) {
        qCWarning(KRDP) << "Could not stop Clipboard context";
        return;
    };
    d->enabled = false;
}

void Clipboard::setServerData(const QMimeData *data)
{
    d->serverData.reset(data);
    QMetaObject::invokeMethod(this, &Clipboard::sendServerData, Qt::QueuedConnection);
}

std::unique_ptr<QMimeData> Clipboard::getClipboard() const
{
    return std::move(d->clientData);
}

void Clipboard::sendServerData()
{
    if (!d->serverData || !d->enabled) {
        return;
    }
    // held until the client's init handshake completes; flushed from markClientReady()
    if (!d->clientReady) {
        return;
    }

    d->outgoingFiles.clear();
    d->openOutgoingFile.close();
    d->openOutgoingIndex = -1;
    if (d->fileTransferSupported() && d->serverData->hasUrls()) {
        d->collectOutgoingFiles(d->serverData->urls());
    }

    QList<CLIPRDR_FORMAT> formats;
    QByteArray fileGroupDescriptorName = FileGroupDescriptorName.toString().toLatin1();
    QByteArray fileContentsName = FileContentsName.toString().toLatin1();
    if (!d->outgoingFiles.isEmpty()) {
        formats.append(CLIPRDR_FORMAT{.formatId = ServerFileGroupDescriptorId, .formatName = fileGroupDescriptorName.data()});
        formats.append(CLIPRDR_FORMAT{.formatId = ServerFileContentsId, .formatName = fileContentsName.data()});
    }
    if (d->serverData->hasText()) {
        formats.append(CLIPRDR_FORMAT{.formatId = CF_UNICODETEXT, .formatName = nullptr});
    }
    if (formats.isEmpty()) {
        return;
    }

    CLIPRDR_FORMAT_LIST formatList = {};
    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.numFormats = formats.size();
    formatList.formats = formats.data();
    d->clipContext->ServerFormatList(d->clipContext.get(), &formatList);
}

uint32_t Clipboard::Private::onClientCapabilities(const CLIPRDR_CAPABILITIES *)
{
    // covers copy-out with no prior copy-in: mstsc sends no format list when its clipboard starts empty
    markClientReady();
    return CHANNEL_RC_OK;
}

uint32_t Clipboard::Private::onClientFormatList(const CLIPRDR_FORMAT_LIST *formatList)
{
    // fallback readiness signal for clients that reach us with a format list first
    markClientReady();

    // The client's clipboard changed, whatever we were still fetching from it is gone.
    if (incoming) {
        abortIncomingTransfer(u"the client's clipboard changed"_s);
    }

    clientFileGroupDescriptorId = 0;
    bool hasText = false;
    for (uint32_t i = 0; i < formatList->numFormats; ++i) {
        const auto &format = formatList->formats[i];
        switch (format.formatId) {
        case CF_TEXT:
        case CF_UNICODETEXT:
        case CF_OEMTEXT:
            hasText = true;
            break;
        default:
            if (format.formatName && FileGroupDescriptorName == QLatin1StringView(format.formatName)) {
                clientFileGroupDescriptorId = format.formatId;
            }
            break;
        }
    }

    // Acknowledge the client's format list (required by CLIPRDR protocol)
    CLIPRDR_FORMAT_LIST_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    clipContext->ServerFormatListResponse(clipContext.get(), &response);

    // Copied files come with formats of their names as text too, the files are what matters.
    if (clientFileGroupDescriptorId != 0 && fileTransferSupported()) {
        requestFormatData(clientFileGroupDescriptorId);
    } else if (hasText) {
        requestFormatData(CF_UNICODETEXT);
    }

    return CHANNEL_RC_OK;
}

void Clipboard::Private::requestFormatData(uint32_t formatId)
{
    pendingFormatRequest = formatId;
    CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest{.common = CLIPRDR_HEADER({.msgType = CB_FORMAT_DATA_REQUEST, .msgFlags = 0, .dataLen = 4}),
                                                  .requestedFormatId = formatId};
    clipContext->ServerFormatDataRequest(clipContext.get(), &formatDataRequest);
}

uint32_t Clipboard::Private::onClientFormatListResponse(const CLIPRDR_FORMAT_LIST_RESPONSE *formatListResponse)
{
    Q_UNUSED(formatListResponse);
    return CHANNEL_RC_OK;
}

void Clipboard::Private::sendFormatDataResponse(const QByteArray &data, bool ok)
{
    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.common.dataLen = ok ? data.size() : 0;
    response.requestedFormatData = ok ? reinterpret_cast<const BYTE *>(data.constData()) : nullptr;
    clipContext->ServerFormatDataResponse(clipContext.get(), &response);
}

uint32_t Clipboard::Private::onClientFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest)
{
    if (formatDataRequest->requestedFormatId == ServerFileGroupDescriptorId && !outgoingFiles.isEmpty()) {
        const QByteArray fileList = serializeOutgoingFiles();
        sendFormatDataResponse(fileList, !fileList.isEmpty());
        return CHANNEL_RC_OK;
    }

    if (!serverData || formatDataRequest->requestedFormatId != CF_UNICODETEXT) {
        sendFormatDataResponse({}, false);
        return CHANNEL_RC_OK;
    }

    auto text = serverData->text();
    // CF_UNICODETEXT requires null-terminated UTF-16LE
    QByteArray utf16Data(reinterpret_cast<const char *>(text.utf16()), (text.length() + 1) * 2);
    sendFormatDataResponse(utf16Data, true);

    return CHANNEL_RC_OK;
}

uint32_t Clipboard::Private::onClientFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse)
{
    const auto requestedFormat = std::exchange(pendingFormatRequest, std::nullopt);

    if (!(formatDataResponse->common.msgFlags & CB_RESPONSE_OK)) {
        qCDebug(KRDP) << "The client could not give us clipboard format" << requestedFormat.value_or(0);
        return CHANNEL_RC_OK;
    }

    if (requestedFormat && *requestedFormat != 0 && *requestedFormat == clientFileGroupDescriptorId) {
        startIncomingTransfer(QByteArray(reinterpret_cast<const char *>(formatDataResponse->requestedFormatData), formatDataResponse->common.dataLen));
        return CHANNEL_RC_OK;
    }

    // dataLen is unsigned; guard before subtracting the null terminator or a short response underflows
    if (formatDataResponse->common.dataLen < 2 || !formatDataResponse->requestedFormatData) {
        clientData.reset();
        Q_EMIT q->clientDataChanged();
        return CHANNEL_RC_OK; // empty or malformed
    }
    const int nCharacters = int(formatDataResponse->common.dataLen / 2 - 1); // each char16_t is 2 bytes, plus null terminator

    clientData.reset(new QMimeData());

    clientData->setText(QString::fromUtf16(reinterpret_cast<const char16_t *>(formatDataResponse->requestedFormatData), nCharacters));
    Q_EMIT q->clientDataChanged();

    return CHANNEL_RC_OK;
}

void Clipboard::Private::startIncomingTransfer(const QByteArray &fileList)
{
    FILEDESCRIPTORW *descriptors = nullptr;
    UINT32 count = 0;
    if (cliprdr_parse_file_list(reinterpret_cast<const BYTE *>(fileList.constData()), fileList.size(), &descriptors, &count) != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "Could not read the list of files copied on the client";
        return;
    }

    auto transfer = std::make_unique<IncomingTransfer>();
    quint64 totalSize = 0;
    for (UINT32 i = 0; i < count; ++i) {
        const auto &descriptor = descriptors[i];
        const auto nameLength = std::find(std::begin(descriptor.cFileName), std::end(descriptor.cFileName), 0) - std::begin(descriptor.cFileName);
        const QString name = QString::fromUtf16(reinterpret_cast<const char16_t *>(descriptor.cFileName), nameLength);

        IncomingFile file;
        file.relativePath = sanitizedRelativePath(name);
        if (file.relativePath.isEmpty()) {
            qCWarning(KRDP) << "Not taking copied files from the client, it sent an unusable file name:" << name;
            free(descriptors);
            return;
        }
        file.directory = (descriptor.dwFlags & FD_ATTRIBUTES) && (descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (!file.directory && (descriptor.dwFlags & FD_FILESIZE)) {
            file.size = quint64(descriptor.nFileSizeHigh) << 32 | descriptor.nFileSizeLow;
            totalSize += *file.size;
        }
        transfer->files.append(file);
    }
    free(descriptors);

    if (transfer->files.isEmpty()) {
        return;
    }
    if (totalSize > MaximumIncomingSize) {
        qCWarning(KRDP) << "Not taking" << transfer->files.size() << "copied files from the client, they are" << totalSize << "bytes, more than"
                        << MaximumIncomingSize;
        return;
    }

    // Keep the files of the transfer before this one, something may still be copying them, and
    // remove any older ones.
    const QString base = cacheDirectory();
    QDir baseDir(base);
    if (!baseDir.mkpath(u"."_s)) {
        qCWarning(KRDP) << "Could not create" << base;
        return;
    }
    auto previous = baseDir.entryInfoList({u"transfer-*"_s}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (qsizetype i = 1; i < previous.size(); ++i) {
        QDir(previous.at(i).absoluteFilePath()).removeRecursively();
    }

    QTemporaryDir directory(base + u"/transfer-XXXXXX"_s);
    if (!directory.isValid()) {
        qCWarning(KRDP) << "Could not create a directory for copied files in" << base << directory.errorString();
        return;
    }
    directory.setAutoRemove(false);
    transfer->directory = directory.path();

    qCDebug(KRDP) << "Fetching" << transfer->files.size() << "copied files," << totalSize << "bytes, into" << transfer->directory;
    incoming = std::move(transfer);
    fetchNextFile();
}

void Clipboard::Private::fetchNextFile()
{
    auto &transfer = *incoming;
    if (transfer.file) {
        transfer.file->close();
        transfer.file.reset();
    }

    const QDir directory(transfer.directory);
    while (++transfer.current < transfer.files.size()) {
        const auto &entry = transfer.files.at(transfer.current);
        const QString path = directory.filePath(entry.relativePath);
        if (entry.directory) {
            if (!directory.mkpath(entry.relativePath)) {
                abortIncomingTransfer(u"could not create "_s + path);
                return;
            }
            continue;
        }

        if (!directory.mkpath(QFileInfo(entry.relativePath).path())) {
            abortIncomingTransfer(u"could not create the directory of "_s + path);
            return;
        }
        transfer.file = std::make_unique<QFile>(path);
        if (!transfer.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            abortIncomingTransfer(u"could not write "_s + path);
            return;
        }
        transfer.offset = 0;

        if (!entry.size) {
            transfer.sizeRequested = true;
            requestFileContents(FILECONTENTS_SIZE, 0, 8);
            return;
        }
        if (*entry.size > 0) {
            requestFileContents(FILECONTENTS_RANGE, 0, uint32_t(std::min<quint64>(FileChunkSize, *entry.size)));
            return;
        }
        transfer.file->close();
        transfer.file.reset();
    }

    finishIncomingTransfer();
}

void Clipboard::Private::requestFileContents(uint32_t flags, quint64 position, uint32_t size)
{
    incoming->streamId = nextStreamId++;

    CLIPRDR_FILE_CONTENTS_REQUEST request = {};
    request.common.msgType = CB_FILECONTENTS_REQUEST;
    request.streamId = incoming->streamId;
    request.listIndex = uint32_t(incoming->current);
    request.dwFlags = flags;
    request.nPositionLow = uint32_t(position & 0xFFFFFFFF);
    request.nPositionHigh = uint32_t(position >> 32);
    request.cbRequested = size;
    request.haveClipDataId = FALSE;
    if (clipContext->ServerFileContentsRequest(clipContext.get(), &request) != CHANNEL_RC_OK) {
        abortIncomingTransfer(u"could not ask the client for the file contents"_s);
    }
}

uint32_t Clipboard::Private::onClientFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE *response)
{
    if (!incoming || response->streamId != incoming->streamId) {
        return CHANNEL_RC_OK;
    }
    if (!(response->common.msgFlags & CB_RESPONSE_OK)) {
        abortIncomingTransfer(u"the client could not give us the file contents"_s);
        return CHANNEL_RC_OK;
    }

    auto &transfer = *incoming;
    auto &entry = transfer.files[transfer.current];

    if (std::exchange(transfer.sizeRequested, false)) {
        if (response->cbRequested < 8 || !response->requestedData) {
            abortIncomingTransfer(u"the client sent an invalid file size"_s);
            return CHANNEL_RC_OK;
        }
        quint64 size = 0;
        memcpy(&size, response->requestedData, sizeof(size)); // little endian, like us
        entry.size = size;
        if (size == 0) {
            fetchNextFile();
        } else {
            requestFileContents(FILECONTENTS_RANGE, 0, uint32_t(std::min<quint64>(FileChunkSize, size)));
        }
        return CHANNEL_RC_OK;
    }

    const quint64 remaining = *entry.size - transfer.offset;
    const quint64 length = std::min<quint64>(response->cbRequested, remaining);
    if (length == 0 || !response->requestedData) {
        abortIncomingTransfer(u"the client sent no file contents"_s);
        return CHANNEL_RC_OK;
    }
    if (transfer.file->write(reinterpret_cast<const char *>(response->requestedData), qint64(length)) != qint64(length)) {
        abortIncomingTransfer(u"could not write "_s + transfer.file->fileName());
        return CHANNEL_RC_OK;
    }
    transfer.offset += length;
    transfer.received += length;

    if (transfer.offset < *entry.size) {
        requestFileContents(FILECONTENTS_RANGE, transfer.offset, uint32_t(std::min<quint64>(FileChunkSize, *entry.size - transfer.offset)));
    } else {
        fetchNextFile();
    }
    return CHANNEL_RC_OK;
}

void Clipboard::Private::abortIncomingTransfer(const QString &reason)
{
    qCWarning(KRDP) << "Not taking the files copied on the client:" << reason;
    if (incoming->file) {
        incoming->file->close();
    }
    QDir(incoming->directory).removeRecursively();
    incoming.reset();
}

void Clipboard::Private::finishIncomingTransfer()
{
    auto transfer = std::move(incoming);

    // The top level files and directories are what was copied.
    QList<QUrl> urls;
    for (const auto &entry : std::as_const(transfer->files)) {
        if (!entry.relativePath.contains(u'/')) {
            urls.append(QUrl::fromLocalFile(QDir(transfer->directory).filePath(entry.relativePath)));
        }
    }

    qCInfo(KRDP) << "Received" << transfer->files.size() << "copied files," << transfer->received << "bytes, from the client";

    QByteArray copiedFiles = "copy";
    for (const auto &url : std::as_const(urls)) {
        copiedFiles += '\n' + url.toEncoded();
    }

    clientData.reset(new QMimeData());
    clientData->setUrls(urls);
    // so file managers know to copy rather than move them
    clientData->setData(u"x-special/gnome-copied-files"_s, copiedFiles);
    clientData->setData(u"application/x-kde-cutselection"_s, "0");
    Q_EMIT q->clientDataChanged();
}

void Clipboard::Private::collectOutgoingFiles(const QList<QUrl> &urls)
{
    auto add = [this](const QFileInfo &info, const QString &name) {
        if (name.size() > MaximumFileNameLength) {
            qCWarning(KRDP) << "Not offering" << info.absoluteFilePath() << "to the client, its path is too long";
            return;
        }
        outgoingFiles.append(OutgoingFile{
            .localPath = info.absoluteFilePath(),
            .name = name,
            .directory = info.isDir(),
            .size = info.isDir() ? 0 : quint64(info.size()),
            .lastModified = info.lastModified(),
        });
    };

    const QString ownFiles = cacheDirectory() + u'/';
    for (const auto &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        // files we received from the client, no use sending them back
        if (info.absoluteFilePath().startsWith(ownFiles)) {
            continue;
        }
        if (!info.exists()) {
            continue;
        }
        add(info, info.fileName());
        if (!info.isDir()) {
            continue;
        }
        // Directories are sent as everything in them, with paths relative to the copied one.
        const QDir parent = info.dir();
        QDirIterator it(info.absoluteFilePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo child = it.nextFileInfo();
            add(child, parent.relativeFilePath(child.absoluteFilePath()).replace(u'/', u'\\'));
        }
    }
}

QByteArray Clipboard::Private::serializeOutgoingFiles() const
{
    std::vector<FILEDESCRIPTORW> descriptors(outgoingFiles.size());
    for (qsizetype i = 0; i < outgoingFiles.size(); ++i) {
        const auto &file = outgoingFiles.at(i);
        auto &descriptor = descriptors[i];
        descriptor = {};
        descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
        descriptor.dwFileAttributes = file.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        descriptor.nFileSizeHigh = uint32_t(file.size >> 32);
        descriptor.nFileSizeLow = uint32_t(file.size & 0xFFFFFFFF);
        const quint64 writeTime = toFileTime(file.lastModified);
        descriptor.ftLastWriteTime.dwHighDateTime = uint32_t(writeTime >> 32);
        descriptor.ftLastWriteTime.dwLowDateTime = uint32_t(writeTime & 0xFFFFFFFF);
        std::copy_n(reinterpret_cast<const WCHAR *>(file.name.utf16()), file.name.size(), descriptor.cFileName);
    }

    uint32_t flags = CB_STREAM_FILECLIP_ENABLED;
    if (clipContext->fileClipNoFilePaths) {
        flags |= CB_FILECLIP_NO_FILE_PATHS;
    }
    if (clipContext->hasHugeFileSupport) {
        flags |= CB_HUGE_FILE_SUPPORT_ENABLED;
    }

    BYTE *data = nullptr;
    UINT32 length = 0;
    if (cliprdr_serialize_file_list_ex(flags, descriptors.data(), descriptors.size(), &data, &length) != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "Could not put together the list of copied files for the client";
        return {};
    }
    QByteArray result(reinterpret_cast<const char *>(data), length);
    free(data);
    return result;
}

void Clipboard::Private::sendFileContentsResponse(uint32_t streamId, const QByteArray &data, bool ok)
{
    CLIPRDR_FILE_CONTENTS_RESPONSE response = {};
    response.common.msgType = CB_FILECONTENTS_RESPONSE;
    response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.common.dataLen = ok ? 4 + data.size() : 4;
    response.streamId = streamId;
    response.cbRequested = ok ? data.size() : 0;
    response.requestedData = ok ? reinterpret_cast<const BYTE *>(data.constData()) : nullptr;
    clipContext->ServerFileContentsResponse(clipContext.get(), &response);
}

uint32_t Clipboard::Private::onClientFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST *request)
{
    if (request->listIndex >= uint32_t(outgoingFiles.size())) {
        sendFileContentsResponse(request->streamId, {}, false);
        return CHANNEL_RC_OK;
    }
    const auto &file = outgoingFiles.at(request->listIndex);

    if (request->dwFlags & FILECONTENTS_SIZE) {
        const quint64 size = file.size;
        sendFileContentsResponse(request->streamId, QByteArray(reinterpret_cast<const char *>(&size), sizeof(size)), true);
        return CHANNEL_RC_OK;
    }

    if (!(request->dwFlags & FILECONTENTS_RANGE) || file.directory) {
        sendFileContentsResponse(request->streamId, {}, false);
        return CHANNEL_RC_OK;
    }

    if (openOutgoingIndex != qsizetype(request->listIndex)) {
        openOutgoingFile.close();
        openOutgoingFile.setFileName(file.localPath);
        if (!openOutgoingFile.open(QIODevice::ReadOnly)) {
            qCWarning(KRDP) << "Could not read" << file.localPath << "for the client";
            openOutgoingIndex = -1;
            sendFileContentsResponse(request->streamId, {}, false);
            return CHANNEL_RC_OK;
        }
        openOutgoingIndex = request->listIndex;
    }

    const quint64 position = quint64(request->nPositionHigh) << 32 | request->nPositionLow;
    if (!openOutgoingFile.seek(qint64(position))) {
        sendFileContentsResponse(request->streamId, {}, false);
        return CHANNEL_RC_OK;
    }
    const QByteArray data = openOutgoingFile.read(std::min<qint64>(request->cbRequested, FileChunkSize * 8));
    sendFileContentsResponse(request->streamId, data, true);
    return CHANNEL_RC_OK;
}
}
