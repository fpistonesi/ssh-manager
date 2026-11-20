# Patch per qtermwidget

Modifiche a [lxqt/qtermwidget](https://github.com/lxqt/qtermwidget) usate da SSH Manager,
generate con `git format-patch` a partire dal commit upstream
`bcd7594428e2d7368b1e957110457e0b610b01b5` ("Weblate commits (#694)").

| Patch | Contenuto | Serve per |
|---|---|---|
| `0001-Windows-port-ConPTY-backend.patch` | Backend pseudo console di Windows (`Pty_win`) al posto di kpty/kprocess; rami Win32 in Session, History, BlockArray, Screen (coppie surrogate UTF-16), Emulation, konsole_wcwidth, tools; CMake senza kpty/utempter. | Compilare ed eseguire su Windows 10 1809+ |
| `0002-Add-QTermWidget-cursorLineText-and-isAlternateScreen.patch` | `QTermWidget::cursorLineText()` e `isAlternateScreen()`. | Cronologia automatica dei comandi (su ogni piattaforma) |
| `0003-Add-exit-code-access-and-optional-exit-dialog.patch` | `QTermWidget::exitCode()`, `exitCrashed()` e `setExitDialogEnabled()`. | Codice di uscita nella barra di fine sessione, senza la finestra "exited unexpectedly" di qtermwidget |

Senza le patch 0002 e 0003 SSH Manager si compila lo stesso: CMake se ne accorge e disattiva
solo le parti che le richiedono (registrazione automatica dei comandi digitati; codice di
uscita, mentre il motivo della fine sessione viene comunque ricavato dall'output).

## Applicazione

```bash
git clone https://github.com/lxqt/qtermwidget.git
cd qtermwidget
git checkout bcd7594428e2d7368b1e957110457e0b610b01b5
git am /percorso/ssh-manager/patches/qtermwidget/*.patch
```

Poi si compila e si installa in un prefisso (ad esempio `deps\prefix`), insieme a
`lxqt2-build-tools`, come descritto nel README principale.

## Rigenerare le patch

Nel clone di qtermwidget le modifiche stanno sul ramo `windows-port`:

```bash
git format-patch master..windows-port -o /percorso/ssh-manager/patches/qtermwidget
```
