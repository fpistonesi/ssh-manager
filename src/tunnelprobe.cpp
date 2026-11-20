#include "tunnelprobe.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <vector>
#else
#include <QFile>
#include <QStringList>
#endif

namespace tunnelprobe {

#ifdef _WIN32
namespace {

// Fills `ports` from GetExtendedTcpTable for one address family.
bool collect(ULONG family, qint64 ownerPid, QSet<quint16> &ports)
{
    DWORD size = 0;
    DWORD result = GetExtendedTcpTable(nullptr, &size, FALSE, family,
                                       TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<unsigned char> buffer;
    // The table can grow between the two calls: retry a few times.
    for (int attempt = 0; attempt < 4 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        buffer.resize(size);
        result = GetExtendedTcpTable(buffer.data(), &size, FALSE, family,
                                     TCP_TABLE_OWNER_PID_LISTENER, 0);
    }
    if (result != NO_ERROR)
        return false;

    if (family == AF_INET) {
        const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_TCPROW_OWNER_PID &row = table->table[i];
            if (ownerPid <= 0 || qint64(row.dwOwningPid) == ownerPid)
                ports.insert(ntohs(static_cast<u_short>(row.dwLocalPort)));
        }
    } else {
        const auto *table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID *>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_TCP6ROW_OWNER_PID &row = table->table[i];
            if (ownerPid <= 0 || qint64(row.dwOwningPid) == ownerPid)
                ports.insert(ntohs(static_cast<u_short>(row.dwLocalPort)));
        }
    }
    return true;
}

} // namespace

QSet<quint16> listeningPorts(qint64 ownerPid, bool *supported)
{
    QSet<quint16> ports;
    const bool v4 = collect(AF_INET, ownerPid, ports);
    const bool v6 = collect(AF_INET6, ownerPid, ports);
    if (supported)
        *supported = v4 || v6;
    return ports;
}

#else

QSet<quint16> listeningPorts(qint64 ownerPid, bool *supported)
{
    Q_UNUSED(ownerPid)
    // /proc/net/tcp{,6}: "sl local_address rem_address st ...", with the
    // address as HEX_IP:HEX_PORT and state 0A meaning LISTEN.
    QSet<quint16> ports;
    bool any = false;
    for (const char *path : {"/proc/net/tcp", "/proc/net/tcp6"}) {
        QFile file(QString::fromLatin1(path));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        any = true;
        file.readLine(); // header
        while (!file.atEnd()) {
            const QStringList fields =
                QString::fromLatin1(file.readLine()).simplified().split(QLatin1Char(' '));
            if (fields.size() < 4 || fields.at(3) != QLatin1String("0A"))
                continue;
            const QString local = fields.at(1);
            bool ok = false;
            const uint port = local.mid(local.lastIndexOf(QLatin1Char(':')) + 1).toUInt(&ok, 16);
            if (ok && port <= 0xFFFF)
                ports.insert(static_cast<quint16>(port));
        }
    }
    if (supported)
        *supported = any;
    return ports;
}

#endif

} // namespace tunnelprobe
