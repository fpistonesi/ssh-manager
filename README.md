# SSH Manager

A desktop SSH session manager for **Linux** and **Windows**, written in C++17 with **Qt 6**
and **QTermWidget**, built on top of the system **OpenSSH** client.

Connections, passwords and private keys live in a single encrypted vault protected by a
master password. Sessions open in tabs or MDI windows with a full terminal, and the app adds
the tools you usually reach for around SSH: file transfer, tunnels, jump hosts, batch
commands, snippets and host health checks.

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
![Qt 6](https://img.shields.io/badge/Qt-6-41cd52.svg)
![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey.svg)

## Features

**Vault and security**
- Single encrypted vault (`vault.db`) for connections, credentials and private keys:
  AES-256-GCM, with the key derived through PBKDF2-HMAC-SHA256 (600,000 iterations, random salt).
- Master password, which can be changed (re-encrypted with a new key and salt).
- Auto-lock after inactivity (1–60 minutes or never) and manual lock with `Ctrl+L`. On lock,
  sessions are closed and decrypted data is wiped from memory.
- Private keys can be imported into the vault, so they never sit unencrypted on disk.
- Host key verification on first connection (SHA-256 fingerprint) and a known-hosts manager
  that shows the fingerprint, first-seen date and the connections that use each key.

**Connections**
- Tree of folders and connections with:
  - search and filters (favourites, tags);
  - drag-and-drop reordering, or natural alphabetical sorting;
  - optional Destination, Favourite (★) and Tags columns.
- Authentication by password, private key (with passphrase) or ssh-agent.
- Recursive jump hosts: each hop is another saved connection with its own credentials.
- Command palette (`Ctrl+K`) with fuzzy search over connections, actions and recent hosts.
  Typing `user@host:port` opens a throwaway session.
- Selective export and import of encrypted archives, to share a few hosts with a colleague.
  Credentials are optional, and you choose how to handle duplicates.
- OpenSSH interoperability:
  - import and export of `~/.ssh/config` (`Host *`, wildcards, `Include`, `ProxyJump` and forwards);
  - key generation with `ssh-keygen` (ed25519 or RSA 4096);
  - an `ssh-copy-id`-style "copy public key to host".

**Sessions and terminal**
- Tabbed or MDI view, with dockable panels whose layout is saved.
- Session end bar showing the reason, exit code and duration, with one-click **Reconnect**.
- Broadcast input to several sessions at once (`Ctrl+Shift+B`).
- Snippets with variables (`${host}`, `${user}`, `${port}`, …) and per-host command history.
- Scrollback search, clickable links, copy-on-select, and activity/bell indicators.
- Light, dark and system themes, switched live.
- English and Italian user interface.

**Tools**
- **File transfer** (`Ctrl+Shift+F`) over SFTP: a two-pane browser with a parallel queue,
  resume and retry, rename, delete and chmod, drag and drop, editing remote files with a
  local editor, and bookmarks.
- **Tunnels**:
  - local (`-L`), remote (`-R`) and dynamic SOCKS (`-D`);
  - run as background services with status, automatic restart and a tray icon.
- **Batch execution** (`Ctrl+Shift+E`): run a command or snippet on many hosts and collect
  each host's exit code, stdout and stderr. Results can be saved as text or JSON.
- **Host health**: load, memory, disks, failed systemd units and top processes, read with a
  single SSH command. Nothing needs to be installed on the server.
- **Reachability** indicator in the tree, showing latency and server version.
- **External tools** (ping, traceroute, port test, FTP, browser, …) launched on a connection
  with host/port/user variables.

See [ROADMAP.md](ROADMAP.md) for what is planned (in Italian).

## Requirements

| | Linux | Windows | macOS |
|---|---|---|---|
| OS | any recent distribution | Windows 10 1809+ / 11 (x64) | untested |
| Qt | 6.2+ (Core, Gui, Widgets, Network, Svg, LinguistTools) | 6.x MinGW (6.10.3 used) | 6.2+ |
| Terminal | `qtermwidget6` (LXQt 2.x) | `qtermwidget` with the ConPTY patch | `qtermwidget6` |
| Crypto | OpenSSL 3 (libcrypto) | OpenSSL 3 | OpenSSL 3 |
| Build | CMake 3.16+, C++17 compiler | CMake, Ninja, MinGW 13.1 | CMake, Xcode CLT |
| Runtime | `ssh` (OpenSSH client) | Windows' built-in OpenSSH client | `ssh` |

The app calls the OpenSSH client for every connection, so `ssh` must be installed and on the
`PATH`. File transfer needs the standard `sftp` subsystem on the server.

### QTermWidget patches

[`patches/qtermwidget`](patches/qtermwidget/README.md) contains three patches for
[lxqt/qtermwidget](https://github.com/lxqt/qtermwidget):

| Patch | Needed for | Without it |
|---|---|---|
| `0001` ConPTY backend | **Windows** (required) | does not build on Windows |
| `0002` `cursorLineText()` | automatic per-host command history | history is not recorded |
| `0003` exit code access | exit code in the session end bar | the reason is still read from the ssh output |

CMake detects whether `0002` and `0003` are present and disables only the matching features,
so a distribution's stock `qtermwidget6` package works on Linux.

## Building on Linux

### 1. Dependencies

Arch / Manjaro:

```bash
sudo pacman -S --needed base-devel cmake qt6-base qt6-svg qt6-tools qtermwidget openssh openssl
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake qt6-qtbase-devel qt6-qtsvg-devel qt6-qttools-devel \
                 qtermwidget-devel openssl-devel openssh-clients
```

Debian / Ubuntu:

```bash
sudo apt install build-essential cmake qt6-base-dev qt6-svg-dev qt6-tools-dev \
                 qt6-tools-dev-tools qt6-l10n-tools libssl-dev openssh-client
```

The Qt 6 QTermWidget development package is only available on recent releases (LXQt 2.x,
for example Debian 13). Look it up with `apt search qtermwidget`. If only the Qt 5 version is
available, build QTermWidget from source (next step).

### 2. QTermWidget from source (optional)

Building QTermWidget from source is needed when your distribution has no Qt 6 package, or
when you want the command history and exit code features:

```bash
git clone https://github.com/lxqt/lxqt-build-tools.git
cmake -S lxqt-build-tools -B lxqt-build-tools/build -DCMAKE_INSTALL_PREFIX="$HOME/.local/sshm-deps"
cmake --build lxqt-build-tools/build && cmake --install lxqt-build-tools/build

git clone https://github.com/lxqt/qtermwidget.git
cd qtermwidget
git checkout bcd7594428e2d7368b1e957110457e0b610b01b5
git am /path/to/ssh-manager/patches/qtermwidget/*.patch
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$HOME/.local/sshm-deps" -DCMAKE_PREFIX_PATH="$HOME/.local/sshm-deps"
cmake --build build -j"$(nproc)" && cmake --install build
```

Then add `-DCMAKE_PREFIX_PATH="$HOME/.local/sshm-deps"` to the configure command below, and
make sure the library is found at run time, for example with
`export LD_LIBRARY_PATH="$HOME/.local/sshm-deps/lib:$LD_LIBRARY_PATH"`.

### 3. Build and run

```bash
git clone <repository-url> ssh-manager
cd ssh-manager
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/sshmanager
```

`sudo cmake --install build` installs the executable in `/usr/local/bin`.

## Building on Windows

Windows needs QTermWidget with the **ConPTY** backend (patch `0001`). The toolchain is the
MinGW build of Qt, installed with [aqtinstall](https://github.com/miurahr/aqtinstall)
(`pip install aqtinstall`).

The commands below assume that everything lives in one root folder, called `$R`, with this
layout:

```
$R\
├── 6.10.3\mingw_64\          Qt
├── Tools\                    MinGW, CMake, Ninja, OpenSSL (from aqt)
├── deps\prefix\              lxqt-build-tools + patched qtermwidget
├── qtermwidget\              qtermwidget sources (for the licence files)
└── ssh-manager\              this repository
```

### 1. Toolchain

```powershell
cd $R
aqt install-qt windows desktop 6.10.3 win64_mingw --outputdir .
aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 --outputdir .
aqt install-tool windows desktop tools_cmake qt.tools.cmake --outputdir .
aqt install-tool windows desktop tools_ninja qt.tools.ninja --outputdir .
aqt install-tool windows desktop tools_opensslv3_x64 qt.tools.opensslv3.win_x64 --outputdir .

$env:PATH = "$R\Tools\mingw1310_64\bin;$R\Tools\CMake_64\bin;$R\Tools\Ninja;$R\6.10.3\mingw_64\bin;$env:PATH"
```

### 2. QTermWidget with the ConPTY patch

```powershell
git clone https://github.com/lxqt/lxqt-build-tools.git
cmake -S lxqt-build-tools -B lxqt-build-tools\build -G Ninja -DCMAKE_INSTALL_PREFIX="$R/deps/prefix" `
      -DCMAKE_PREFIX_PATH="$R/6.10.3/mingw_64"
cmake --build lxqt-build-tools\build; cmake --install lxqt-build-tools\build

git clone https://github.com/lxqt/qtermwidget.git
cd qtermwidget
git checkout bcd7594428e2d7368b1e957110457e0b610b01b5
git am ..\ssh-manager\patches\qtermwidget\*.patch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$R/deps/prefix" `
      -DCMAKE_PREFIX_PATH="$R/6.10.3/mingw_64;$R/deps/prefix"
cmake --build build; cmake --install build
cd ..
```

### 3. Build and run

```powershell
cd ssh-manager
cmake -S . -B ..\build-sshmanager -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH="$R/6.10.3/mingw_64;$R/deps/prefix" `
      -DOPENSSL_ROOT_DIR="$R/Tools/OpenSSLv3/Win_x64"
cmake --build ..\build-sshmanager
windeployqt --release --compiler-runtime ..\build-sshmanager\sshmanager.exe
..\build-sshmanager\sshmanager.exe
```

The build copies `libqtermwidget6.dll`, `libcrypto-3-x64.dll`, `kb-layouts` and
`color-schemes` next to the executable, and `windeployqt` adds Qt and the MinGW runtime. The
build folder then runs on its own. The executable carries the application icon and version
information.

### 4. Distributable package

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\package.ps1
```

[`package.ps1`](packaging/windows/package.ps1) does the following:
1. builds a Release build in `$R\build-release`;
2. deploys Qt (English and Italian translations only) and adds the licence files;
3. checks that every required DLL is in the package and that the app starts with only
   `System32` on the `PATH`;
4. writes `$R\dist\SSHManager-<version>-win64.zip`, which needs no installation: unzip it and
   run `sshmanager.exe`. Add a `vault.db` next to the executable to make it fully portable; see
   [Portable mode](#portable-mode).

If [Inno Setup 6](https://jrsoftware.org/isinfo.php) is installed, the script also builds
`SSHManager-<version>-setup.exe` from [`sshmanager.iss`](packaging/windows/sshmanager.iss).
That installer is per user and needs no administrator rights. Uninstalling it keeps the vault
and the settings.

On the target PC, the only requirement is the Windows OpenSSH client, which is installed by
default on Windows 10 1809 and later (*Settings › Optional features › OpenSSH Client*).

## Building on macOS (untested)

macOS is not an officially supported platform yet and has not been tested. The code has no
Windows-only parts outside `#ifdef _WIN32`, and QTermWidget supports macOS, so a build
should work as follows:

```bash
brew install cmake ninja qt openssl@3
# lxqt-build-tools and qtermwidget from source, as in the Linux section,
# with -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$HOME/.local/sshm-deps"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$HOME/.local/sshm-deps" \
      -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build
./build/sshmanager
```

Known limitation: the tunnel listening check reads `/proc/net/tcp`, which macOS lacks, so
tunnel status relies only on the ssh output. Reports and patches are welcome.

## Getting started

1. On first launch, create the master password. It cannot be recovered: without it the vault
   cannot be decrypted.
2. Create a folder (*New folder*), then a connection (`Ctrl+Shift+N`). Alternatively, import
   your existing hosts from *Vault › OpenSSH › Import from ~/.ssh/config*.
3. Double-click a connection (or press `Ctrl+Enter`) to open a session.
4. Press `Ctrl+K` at any time to search connections and actions, or to type `user@host:port`.

| Shortcut | Action |
|---|---|
| `Ctrl+K` | Command palette / quick connect |
| `Ctrl+Shift+N` | New connection |
| `Ctrl+Enter` | Connect |
| `Ctrl+Shift+T` | Local terminal |
| `Ctrl+Shift+F` | File transfer |
| `Ctrl+Shift+E` | Batch execution |
| `Ctrl+Shift+B` | Broadcast input |
| `Ctrl+Shift+S` | Toggle favourite |
| `Ctrl+W` / `Ctrl+PgUp` / `Ctrl+PgDown` | Close / previous / next session |
| `Ctrl+L` | Lock the vault |

The interface language follows the system: Italian on Italian systems, English elsewhere. You
can change it from *View › Appearance › Language*.

## Where data is stored

| | Linux | Windows |
|---|---|---|
| Encrypted vault | `~/.config/sshmanager/vault.db` | `%LOCALAPPDATA%\sshmanager\vault.db` |
| Accepted host keys | `~/.config/sshmanager/known_hosts` | `%LOCALAPPDATA%\sshmanager\known_hosts` |
| Settings (no secrets) | `~/.config/sshmanager/sshmanager.conf` | registry, `HKCU\Software\sshmanager\sshmanager` |
| Session temp files | `~/.config/sshmanager/run/` (mode `0700`) | `%LOCALAPPDATA%\sshmanager\run\` |

### Portable mode

If a `vault.db` file sits next to the executable, it takes priority over the standard
location. This works for the Windows zip on a USB stick, and for an extracted build on Linux.
In portable mode, these items live in the executable's folder:
- the vault and `known_hosts`;
- the settings, in `sshmanager\sshmanager.ini` (not in the registry).

The window title shows *(portable)*, and the status bar shows *Portable*; hover it to see
the folder in use.

- To use an existing vault, copy your `vault.db` next to `sshmanager.exe`.
- To start a new portable vault, create an **empty** file named `vault.db` there. The app then
  asks for a new master password.

Temporary session files (decrypted keys, askpass answers) are still written to the local
machine's `run/` folder and deleted when the session ends, so secrets never land on the
removable drive. The folder must be writable.

## Security notes

- OpenSSH needs key files and askpass answers on disk. While a session runs, they are written
  to `run/`, readable only by the user (`0600` on Linux, the user profile ACL on Windows), and
  deleted when the session closes. Leftovers from a crash are removed at the next start.
- While the vault is unlocked, decrypted data is necessarily in process memory. Wiping on lock
  is best effort: Qt widgets may keep transient copies of strings.
- New host keys must be confirmed, and a changed host key blocks the connection with a clear
  warning.
- Exported archives are encrypted with their own password. Credentials are excluded unless
  you choose to include them.

## Contributing

Issues and pull requests are welcome. User-visible strings are translated in
`translations/sshmanager_{en,it}.ts` and compiled into the executable at build time.

## License

SSH Manager is free software, released under the **GNU General Public License v3.0**. See
[LICENSE](LICENSE).

Third-party components:
- [Qt](https://www.qt.io/), LGPL v3, dynamically linked;
- [QTermWidget](https://github.com/lxqt/qtermwidget), GPL v2 or later, with BSD-3-Clause and LGPL parts;
- [OpenSSL](https://www.openssl.org/), Apache License 2.0;
- the Windows package also ships the MinGW-w64/GCC runtime (GPL with the runtime exception).
