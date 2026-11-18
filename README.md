# SSH Manager

Gestore di sessioni SSH per desktop Linux, scritto in C++ con **Qt 6 Widgets** e
**QTermWidget** (il terminale di QTerminal), sopra il client **OpenSSH** di sistema.

## Funzionalità

- **Archivio cifrato unico** (`vault.db`) per connessioni, credenziali e chiavi private,
  protetto da una **master password**; l'intero contenuto viene cifrato con AES-256-GCM e la
  chiave è derivata con PBKDF2-HMAC-SHA256 (600.000 iterazioni, salt casuale).
- **Blocco automatico** configurabile (1, 5, 15, 30, 60 minuti oppure mai; default 15) dopo
  l'inattività, più blocco manuale con `Ctrl+L`. Al blocco le sessioni vengono chiuse, la chiave
  e i dati decifrati vengono cancellati dalla memoria (azzeramento con `OPENSSL_cleanse`).
- **Cambio master password** dal menu *Sicurezza*: l'archivio viene ricifrato con una nuova
  chiave e un nuovo salt.
- **Connessioni salvate** in un albero di progetti/cartelle e gruppi annidati, con filtro di
  ricerca.
- **Comando rapido** (`Ctrl+K`): palette con ricerca fuzzy su connessioni, azioni e
  destinazioni recenti. Accetta anche `utente@host:porta` per aprire una sessione
  usa-e-getta senza salvarla; la password viene chiesta nel terminale o si usa l'agent.
- **Preferiti e tag**: stella nell'albero, tag modificabili nella connessione, visibili nel
  tooltip e cercabili nella palette e nel filtro. I preferiti hanno priorità nei risultati
  della palette.
- **Filtri nella barra delle connessioni**: pulsante accanto alla ricerca con *Solo
  preferiti*, filtro per tag e *Azzera filtri*.
- **Interfaccia bilingue**: italiano e inglese, scelti dalla lingua di sistema o da
  `ui/language` in `sshmanager.conf`; i cataloghi sono incorporati in `:/i18n`.
- **Autenticazione** con password, chiave privata (con passphrase) o ssh-agent. Le chiavi
  possono essere **importate nel database**: non restano in file non cifrati.
- **Finestre MDI**: ogni sessione è una sottofinestra ridimensionabile, con affiancamento,
  sovrapposizione e chiusura di tutte.
- **Jump host** ricorsivo: una connessione può passare per un'altra connessione salvata, che a
  sua volta può averne un'altra; la catena usa `ProxyCommand` e ogni hop ha le proprie
  credenziali.
- **Tunnel**: port forwarding locale `-L` (uno o più per connessione), creati attraverso la
  catena di jump host e avviati con la sessione.
- **Trasmissione input**: `Ctrl+Shift+B` invia ogni tasto e ogni incolla alle sessioni
  abilitate; l'etichetta `TX` indica chi riceve l'input.
- **Trasferimento file** (`Ctrl+Shift+F` o *Sessioni → File*): finestra a due pannelli con
  cartelle locali a sinistra e directory remota a destra, invio/ricezione di file e cartelle
  intere, coda con stato per elemento e avanzamento in byte. Il trasferimento usa SFTP sopra
  lo stesso `ssh` delle sessioni, quindi riusa chiavi, password, jump host e `known_hosts`;
  i file remoti vengono inviati/ricevuti a blocchi con possibilità di annullare.
- Copia/incolla, selezione, cronologia e ricerca sono quelli di QTermWidget
  (`Ctrl+Shift+C` / `Ctrl+Shift+V`).

## Requisiti

- Qt 6 (Core, Gui, Widgets), Qt 6 LinguistTools e `qtermwidget6` con header di sviluppo
- Client OpenSSH (`/usr/bin/ssh`) e OpenSSL
- CMake ≥ 3.16 e un compilatore C++17

Su Arch/Manjaro:

```bash
sudo pacman -S --needed qt6-base qt6-tools qtermwidget openssh openssl cmake gcc
```

## Compilazione ed esecuzione

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/sshmanager
```

Al primo avvio viene chiesto di creare la master password. Se esiste un archivio delle
versioni precedenti (`connections.json` con chiave locale) viene proposta la migrazione: dopo
l'importazione i vecchi file vengono eliminati.

## Uso

1. Sblocca l'archivio con la master password.
2. *File → Nuova cartella* per creare un progetto/gruppo.
3. *File → Nuova connessione*: host, utente, metodo di autenticazione, eventuale chiave
   importata, jump host e tunnel. Per una prova veloce premi `Ctrl+K` e digita
   `utente@host:porta`. Con `Ctrl+Shift+S` aggiungi o togli la connessione selezionata dai
   preferiti; il pulsante **Filtri** accanto alla ricerca mostra solo i preferiti, filtra per
   tag o azzera i filtri.
4. Doppio clic (o *Connetti*) per aprire la sessione MDI.
5. Per l'input simultaneo: apri più sessioni, attiva **Trasmetti input** e digita.
6. Per copiare file: seleziona una connessione (o apri la sessione) e premi **Trasferisci
   file**. A sinistra navighi nel filesystem locale, a destra in quello remoto; seleziona gli
   elementi e usa **Invia →** o **← Ricevi**. Doppio clic su una cartella per entrarci, riga del
   percorso + Invio per andare a un percorso preciso.

## Lingua

L'interfaccia segue la lingua di sistema: italiano sui sistemi italiani, inglese altrove.
Per forzarla, imposta `ui/language=it` oppure `ui/language=en` nella sezione `[ui]` di
`~/.config/sshmanager/sshmanager.conf` e riavvia l'app. Le traduzioni dei pulsanti standard
Qt vengono caricate dall'installazione di Qt quando disponibili.

## Dove sono salvati i dati

| Percorso | Contenuto |
| --- | --- |
| `~/.config/sshmanager/vault.db` | archivio cifrato: albero, password, passphrase, chiavi private |
| `~/.config/sshmanager/known_hosts` | chiavi host accettate al primo collegamento (non segrete) |
| `~/.config/sshmanager/sshmanager.conf` | impostazioni non segrete: minuti di blocco automatico, identificativi delle connessioni recenti e destinazioni rapide recenti |
| `~/.config/sshmanager/run/` | file temporanei delle sessioni, dir `0700`, cancellati alla chiusura |

**Sicurezza e limiti**

- La master password non è recuperabile: senza di essa l'archivio non è decifrabile.
- Mentre l'archivio è sbloccato, i dati decifrati sono necessariamente nella memoria del
  processo per poterli mostrare e usare; al blocco (timer, `Ctrl+L`, uscita) vengono azzerati
  e l'albero viene svuotato. Le QString di Qt usate dai widget possono conservare copie
  transitorie non azzerabili, quindi l'azzeramento è best-effort sulle strutture del modello.
- Per l'autenticazione OpenSSH ha bisogno di un file di chiave: le chiavi importate vengono
  scritte in `run/` con permessi `0600` solo per la durata della sessione e rimosse alla
  chiusura. Anche le credenziali per i prompt passano da un file `askpass-*.json` `0600`
  temporaneo, eliminato con la sessione; i file rimasti da un crash vengono rimossi al riavvio.
- Chiavi host nuove: accettate automaticamente (`StrictHostKeyChecking=accept-new`); una chiave
  host cambiata fa fallire la connessione.
- Non sono inclusi port forwarding remoto (`-R`) o dinamico (`-D`), SCP e più istanze
  contemporanee.
- Il trasferimento richiede un server SFTP sul lato remoto (il sottosistema `sftp` standard di
  OpenSSH); in caso contrario la finestra mostra l'errore restituito da `ssh`.
- I collegamenti simbolici non vengono seguiti nelle cartelle copiate, per evitare duplicazioni
  e cicli.
- Il blocco automatico chiude le sessioni aperte. Se un dialogo modale è aperto allo scadere
  del timer, viene chiuso prima del blocco.

## Verifiche eseguite

- Compilazione CMake e avvio offscreen con archivio esistente (dialog di sblocco) e senza
  archivio (dialog di creazione).
- Vault: creazione, nessun segreto in chiaro nel file, permessi `0600`, rifiuto password
  errata, sblocco, salvataggio, cambio password, vecchia password invalidata, salvataggio
  rifiutato da bloccato, wipe dei segreti.
- Migrazione: caricamento di un `connections.json` legacy cifrato con la chiave locale e
  conversione nel nuovo formato.
- Chiave privata incorporata con passphrase: connessione reale a un sshd locale, chiave
  materializzata in `run/` `0600`, prompt passphrase risolto via askpass, file temporanei
  rimossi a fine sessione.
- Autenticazione con chiave, catena jump (`ProxyCommand`) e tunnel `-L` verificati in
  precedenza contro un sshd locale; trasmissione input verificata tra due QTermWidget.
- Trasferimento file: client SFTP v3 provato contro `sftp-server` locale (handshake, elenco,
  creazione cartelle, invio e ricezione di file binari e sottocartelle, contenuti identici) e
  finestra provata offscreen (connessione, navigazione, abilitazione dei pulsanti, invio e
  ricezione dalla coda).
- Comando rapido: parsing di `host`, `utente@host`, `host:porta` e `utente@host:porta`
  verificato, palette provata offscreen (filtro fuzzy su connessioni e azioni, riga di
  connessione rapida, selezione con Invio).
- Preferiti e tag: salvataggio/rilettura nel JSON del vault verificati; priorità dei
  preferiti e ricerca per tag provate offscreen.
- Barra connessioni: pulsante **Filtri** e relative voci (*Solo preferiti*, *Tag*, *Azzera
  filtri*) verificati offscreen; l'albero usa due colonne, senza la colonna Tag.
- Traduzioni: 339 stringhe, 0 incomplete; `lrelease` genera i `.qm` e le risorse
  `:/i18n/sshmanager_it.qm` e `:/i18n/sshmanager_en.qm` sono presenti nel binario.
  Caricamento EN/IT e traduzione di stringhe campione verificati offscreen.
- Non verificati: aspetto visivo/clic dell'interfaccia, login con password su un server reale
  (un sshd non privilegiato non può verificare le password di sistema) e trasferimento verso
  un host remoto reale (i test usano `sftp-server` come processo locale).
