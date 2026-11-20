#pragma once

#include <QString>

namespace paths {

// Portable mode: a vault.db next to the executable takes priority. The vault,
// known_hosts and the settings (sshmanager/sshmanager.ini) then live in the
// executable's folder; temporary session files stay on the local machine.
// Needs a QCoreApplication; the answer is fixed at the first call.
bool isPortable();
QString portableDir();  // empty when not portable

// ~/.config/sshmanager (created with 0700 permissions on first use), or the
// executable's folder in portable mode.
QString configDir();
QString knownHostsFile();
QString storeFile();   // legacy pre-vault store, used only for migration
QString keyFile();     // legacy local key, used only for migration
QString vaultFile();
// Always under the local configuration folder, never on a portable drive:
// it holds the session's temporary key and askpass files.
QString runtimeDir();

} // namespace paths
