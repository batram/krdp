// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "Server.h"

#include <vector>

#include <QCoreApplication>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>

#include <linux/vm_sockets.h>
#include <sys/socket.h>
#include <unistd.h>

#include <freerdp/channels/channels.h>
#include <freerdp/freerdp.h>
#include <winpr/ssl.h>

#include "RdpConnection.h"

#include "krdp_logging.h"

using namespace KRdp;

class KRDP_NO_EXPORT Server::Private
{
public:
    std::vector<std::unique_ptr<RdpConnection>> sessions;
    rdp_settings *settings = nullptr;

    QHostAddress address = QHostAddress::LocalHost;
    quint16 port = 3389;

    quint32 vsockPort = 0;
    bool vsockTrustHost = false;
    int vsockFd = -1;
    std::unique_ptr<QSocketNotifier> vsockNotifier;

    QList<User> users;
    bool usePamAuthentication = false;

    std::filesystem::path tlsCertificate;
    std::filesystem::path tlsCertificateKey;
};

Server::Server(QObject *parent)
    : QTcpServer(parent)
    , d(std::make_unique<Private>())
{
    winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
}

Server::~Server()
{
    stop();
}

bool Server::start()
{
    if (!std::filesystem::exists(d->tlsCertificate) || !std::filesystem::exists(d->tlsCertificateKey)) {
        qCCritical(KRDP).nospace() << "A valid TLS certificate (" << QString::fromStdString(d->tlsCertificate.filename().string()) << ") and key ("
                                   << QString::fromStdString(d->tlsCertificateKey.filename().string()) << ") is required for the server to run!";
        return false;
    }

    if (!listen(d->address, d->port)) {
        // NOTE: We cannot use QTcpServer methods to get the server address and port because it won't initialize them if listen fails.
        qCCritical(KRDP) << "Unable to listen for connections on" << d->address << d->port;
        return false;
    }

    // FreeRDP3 tries to use a global instance of the settings object when
    // initializing a new peer. However, it seems to fail at actually creating a
    // global default instance. So create one here and use that.
    d->settings = freerdp_settings_new(FREERDP_SETTINGS_SERVER_MODE);

    qCDebug(KRDP) << "Listening for connections on" << serverAddress() << serverPort();

    if (d->vsockPort != 0 && !startVsock()) {
        stop();
        return false;
    }
    return true;
}

bool Server::startVsock()
{
    const int fd = ::socket(AF_VSOCK, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        qCCritical(KRDP) << "Unable to create vsock socket:" << strerror(errno);
        return false;
    }

    sockaddr_vm addr{};
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_ANY;
    addr.svm_port = d->vsockPort;
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(fd, 16) < 0) {
        qCCritical(KRDP) << "Unable to listen for connections on vsock port" << d->vsockPort << ":" << strerror(errno);
        ::close(fd);
        return false;
    }

    d->vsockFd = fd;
    d->vsockNotifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
    connect(d->vsockNotifier.get(), &QSocketNotifier::activated, this, [this]() {
        while (true) {
            sockaddr_vm peer{};
            socklen_t len = sizeof(peer);
            const int client = ::accept4(d->vsockFd, reinterpret_cast<sockaddr *>(&peer), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client < 0) {
                if (errno != EAGAIN && errno != EINTR) {
                    qCWarning(KRDP) << "Failed to accept vsock connection:" << strerror(errno);
                }
                return;
            }
            qCDebug(KRDP) << "Accepted vsock connection from CID" << peer.svm_cid << "port" << peer.svm_port;
            addConnection(client, true);
        }
    });

    qCDebug(KRDP) << "Listening for connections on vsock port" << d->vsockPort << (d->vsockTrustHost ? "(trusting the host for empty logins)" : "");
    return true;
}

void Server::stop()
{
    close();

    d->vsockNotifier.reset();
    if (d->vsockFd >= 0) {
        ::close(d->vsockFd);
        d->vsockFd = -1;
    }

    if (d->settings) {
        freerdp_settings_free(d->settings);
        d->settings = nullptr;
    }
}

QHostAddress Server::address() const
{
    return d->address;
}

void Server::setAddress(const QHostAddress &newAddress)
{
    if (newAddress == d->address) {
        return;
    }

    d->address = newAddress;
}

quint16 Server::port() const
{
    return d->port;
}

void Server::setPort(quint16 newPort)
{
    if (newPort == d->port) {
        return;
    }

    d->port = newPort;
}

quint32 Server::vsockPort() const
{
    return d->vsockPort;
}

void Server::setVsockPort(quint32 newPort)
{
    d->vsockPort = newPort;
}

bool Server::vsockTrustHost() const
{
    return d->vsockTrustHost;
}

void Server::setVsockTrustHost(bool trust)
{
    d->vsockTrustHost = trust;
}

QList<User> KRdp::Server::users() const
{
    return d->users;
}

void KRdp::Server::setUsers(const QList<User> &users)
{
    d->users = users;
}

void KRdp::Server::addUser(const User &user)
{
    d->users.append(user);
}

bool Server::usePAMAuthentication() const
{
    return d->usePamAuthentication;
}

void Server::setUsePAMAuthentication(bool usePAM)
{
    d->usePamAuthentication = usePAM;
}

std::filesystem::path Server::tlsCertificate() const
{
    return d->tlsCertificate;
}

void Server::setTlsCertificate(const std::filesystem::path &newTlsCertificate)
{
    if (newTlsCertificate == d->tlsCertificate) {
        return;
    }

    d->tlsCertificate = newTlsCertificate;
}

std::filesystem::path Server::tlsCertificateKey() const
{
    return d->tlsCertificateKey;
}

void Server::setTlsCertificateKey(const std::filesystem::path &newTlsCertificateKey)
{
    if (newTlsCertificateKey == d->tlsCertificateKey) {
        return;
    }

    d->tlsCertificateKey = newTlsCertificateKey;
}

void Server::incomingConnection(qintptr handle)
{
    addConnection(handle, false);
}

void Server::addConnection(qintptr handle, bool vsock)
{
    auto session = std::make_unique<RdpConnection>(this, handle, vsock);
    auto sessionPtr = session.get();
    // queued: signal comes from the run thread, and it keeps the erase below from destroying the sender mid-emission
    connect(
        sessionPtr,
        &RdpConnection::stateChanged,
        this,
        [this, sessionPtr](RdpConnection::State state) {
            if (state == RdpConnection::State::Closed) {
                auto itr = std::find_if(d->sessions.begin(), d->sessions.end(), [sessionPtr](auto &session) {
                    return session.get() == sessionPtr;
                });
                if (itr == d->sessions.end()) {
                    return;
                }
                // extracted before erasing: ~RdpConnection can spin a nested event loop (KScreen calls
                // from destroyed() handlers) that re-enters this vector, so destroy it once consistent again
                auto session = std::move(*itr);
                d->sessions.erase(itr);
            }
        },
        Qt::QueuedConnection);
    d->sessions.push_back(std::move(session));
    Q_EMIT newConnectionCreated(sessionPtr);
}

rdp_settings *Server::rdpSettings() const
{
    return d->settings;
}

#include "moc_Server.cpp"
