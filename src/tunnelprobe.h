#pragma once

#include <QSet>
#include <QtGlobal>

namespace tunnelprobe {

// Local TCP ports in LISTEN state, read from the OS socket table (no
// connection is ever made, so nothing reaches the remote side).
//
// When ownerPid > 0 only sockets owned by that process are returned, where
// the platform reports owners (Windows); elsewhere every listening port is
// returned. `supported` is set to false when the table cannot be read.
QSet<quint16> listeningPorts(qint64 ownerPid, bool *supported = nullptr);

} // namespace tunnelprobe
