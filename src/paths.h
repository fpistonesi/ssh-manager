#pragma once

#include <QString>

namespace paths {

// ~/.config/sshmanager, created with 0700 permissions on first use.
QString configDir();
QString knownHostsFile();
QString storeFile();   // legacy pre-vault store, used only for migration
QString keyFile();     // legacy local key, used only for migration
QString vaultFile();
QString runtimeDir();

} // namespace paths
