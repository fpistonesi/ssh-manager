# SSHManager — Roadmap

Idee ordinate per valore percepito: frequenza d'uso × dolore rimosso × rischio coperto,
rapportati allo sforzo. Lo stato indica cosa è già stato iniziato.

## Priorità alta

### 1. Sessioni persistenti e riconnessione (tmux/screen)
Opzione per host “usa tmux”; all'apertura `tmux new -A -s sshmanager-<nome>`, pulsante
“Riconnetti” che riapre la stessa sottofinestra, ripristino della disposizione dopo un crash
o un riavvio.

**Perché:** se cade la VPN, il laptop va in sospensione o scatta il blocco automatico, la
sessione termina e il lavoro in corso si perde. Con tmux il processo remoto sopravvive e
l'app diventa il posto da cui riprendere il lavoro, non solo da cui aprirlo.

**Sforzo:** M/L. Nessuna dipendenza nuova lato client; tmux va previsto sul server.

### 2. Autenticazione a più fattori e keyboard-interactive
Supporto a OTP/TOTP, Duo, prompt multipli e `keyboard-interactive`; campo “serve OTP” per
host; gestione di più risposte askpass nella stessa connessione.

**Perché:** oggi per chiave e agent viene forzato `PreferredAuthentications=publickey` e
`NumberOfPasswordPrompts=1`, con un solo segreto passato all'askpass. Gli host con 2FA non
sono raggiungibili o falliscono in modo opaco. È un blocco di compatibilità, non un
abbellimento.

**Sforzo:** S/M, ma richiede di rivedere il protocollo askpass e il modello credenziali.

### 3. Connessione rapida, palette comandi e recenti — COMPLETATO
`Ctrl+K` con ricerca fuzzy su connessioni, azioni e host recenti; barra `utente@host:porta`
per connessioni usa-e-getta; preferiti e tag.

**Perché:** l'app ha oltre 30 azioni e un albero che diventa profondo. La palette elimina la
navigazione ripetitiva e rende utilizzabili anche host non salvati.

**Sforzo:** S/M.

Stato: palette con connessioni, azioni e recenti; connessione rapida
`utente@host[:porta]` con prompt nel terminale; recenti persistiti in `sshmanager.conf`;
preferiti e tag salvati nel vault, visibili nel tooltip, filtrabili dal pulsante Filtri e
cercabili nella palette.
Resta opzionale una sezione “Recenti” dedicata nell'albero: oggi i recenti sono nella
palette.

### 4. Gestore di known_hosts e verifica delle chiavi host
Finestra con host, fingerprint SHA-256, data di primo contatto; rimozione o aggiornamento di
una chiave; verifica esplicita al primo collegamento; messaggio leggibile quando la chiave
cambia.

**Perché:** oggi si usa `StrictHostKeyChecking=accept-new` e non c'è modo di ispezionare o
correggere ciò che è stato accettato. In caso di mismatch l'utente vede solo l'errore di
`ssh`. È un buco di sicurezza e un problema di assistenza molto comune.

**Sforzo:** S/M.

### 5. Snippet e cronologia comandi per host
Libreria di comandi con variabili (`${host}`, `${user}`, `${path}`), invio alla sessione
attiva o a tutte quelle selezionate; cronologia dei comandi digitati per host con “riesegui”;
comando di avvio automatico per connessione.

**Perché:** si integra con il broadcast esistente e trasforma un gestore di sessioni in uno
strumento operativo. Riduce errori su comandi ripetuti.

**Sforzo:** S/M.

### 6. Trasferimento file completo, non solo copia
Crea cartella, rinomina, elimina, permessi; drag & drop tra i due pannelli; ripresa/retry
dopo interruzione; coda con trasferimenti paralleli; modifica di un file remoto con editor
locale e upload al salvataggio; segnalibri.

**Perché:** la finestra attuale sa navigare e copiare, ma per gestire file remoti serve ancora
il terminale. Le operazioni CRUD e il drag & drop sono ciò che la rende uno strumento
quotidiano.

**Sforzo:** M/L.

## Priorità media

### 7. Tunnel completi e gestiti come servizi
`-R` remoto e `-D` SOCKS dinamico; stato per tunnel (attivo/errore), riavvio automatico,
avvio senza aprire un terminale, indicatore nella tray.

**Perché:** oggi esistono solo tunnel locali `-L` creati all'avvio della sessione. Chi
sviluppa usa continuamente SOCKS e reverse tunnel.

**Sforzo:** M.

### 8. Registrazione e audit delle sessioni
Log in formato asciicast (o testo con timestamp), cifrati nel vault o su file con policy per
host; replay; redazione di pattern sensibili; esportazione.

**Perché:** serve per diagnosi, passaggio di consegne e compliance; si sposa con l'architettura
del vault.

**Sforzo:** M.

### 9. Interoperabilità OpenSSH: import `~/.ssh/config`, chiavi, `ssh-copy-id`
Import di `Host`, `HostName`, `User`, `Port`, `ProxyJump`, `IdentityFile`; generazione chiavi
ed25519; pulsante “copia chiave pubblica sull'host”; export verso config OpenSSH.

**Perché:** oggi l'inserimento è manuale e chi ha già un `~/.ssh/config` deve ricopiare tutto.
L'import abbatte la barriera d'ingresso.

**Sforzo:** S/M.

### 10. Fine sessione e notifiche ben gestite
Exit code e motivo della disconnessione, pulsante “Riconnetti”, notifica desktop per sessioni
lunghe o tunnel caduti, indicatore di raggiungibilità host nell'albero.

**Perché:** oggi la sessione finita mostra solo “sessione terminata”. Sapere perché è finita e
poter ripartire con un clic evita di accorgersi tardi di un tunnel morto.

**Sforzo:** S/M.

### 11. Esecuzione batch su host selezionati
Selezione multipla nell'albero, comando da eseguire su tutti, output aggregato per host con
exit code, salvataggio del risultato.

**Perché:** il broadcast è interattivo e non permette di lanciare un comando su un sottoinsieme
raccogliendone l'esito.

**Sforzo:** M.

### 12. Dashboard di salute dell'host
Pannello con load, memoria, disco, uptime, servizi systemd falliti e processi principali, letto
via una singola esecuzione SSH o riusando la sessione.

**Perché:** evita di aprire cinque terminali per capire cosa sta succedendo, senza agent remoti.

**Sforzo:** M.

## Priorità più bassa, ma con valore reale

### 13. Sicurezza locale più comoda
Sblocco tramite keyring di sistema/screen lock (DBus), secondo fattore con file chiave o FIDO2,
svuotamento automatico degli appunti, blocco alla sospensione, policy “blocca l'interfaccia ma
mantieni le sessioni”.

**Perché:** il vault è solido a riposo, ma se l'esperienza è scomoda l'utente sceglie password
deboli o disabilita il blocco.

**Sforzo:** M.

### 14. Finestre staccabili e multi-monitor
Spostare una sessione in una finestra di primo livello, ricordare geometria e schermo, layout
persistente.

**Perché:** l'MDI è ottimo su un monitor, limitante su due.

**Sforzo:** M.

### 15. Internazionalizzazione e accessibilità dell'app principale
Il codice usa già `tr()` ma nel progetto attivo non ci sono `.ts/.qm` né `QTranslator`: le
traduzioni esistono solo nello snapshot Qt Creator. Aggiungere risorse IT/EN, navigazione
completa da tastiera, nomi accessibili e contrasto adeguato.

**Perché:** intervento relativamente economico che allarga il pubblico e migliora la qualità
percepita.

**Sforzo:** S/M.

### 16. Packaging, CI e test automatici
PKGBUILD per Arch/Manjaro, AppImage o Flatpak, GitHub Actions con build headless, test del
vault e del parser SFTP, ASan/UBSan.

**Perché:** il codice contiene crypto e un parser di protocollo scritto a mano: sono le parti
in cui una regressione silenziosa costa cara.

**Sforzo:** M.

### 17. Rifinitura del terminale
Ricerca nello scrollback (da verificare rispetto a quanto già offre QTermWidget), link/IP
cliccabili, copia alla selezione, indicatore di attività/bell per sessione, tema chiaro
coerente.

**Perché:** sono piccoli attriti quotidiani; insieme fanno sembrare l'app molto più curata.

**Sforzo:** S/M.

### 18. Backup versionato, sincronizzazione e igiene del vault
Backup cifrati automatici con rotazione, sincronizzazione opzionale via cartella/cloud
dell'utente, merge in importazione; audit di password/chiavi vecchie o duplicate, fingerprint
delle chiavi.

**Perché:** la perdita o la corruzione del vault significa perdere tutte le credenziali; oggi
l'export è manuale.

**Sforzo:** M.

## Idee che sconsiglio di affrontare ora

- **Port Windows:** il blocco è QTermWidget (nessun supporto Windows) e askpass inaffidabile.
  Richiederebbe di sostituire il terminale e riscrivere l'autenticazione.
- **Plugin/scripting API:** moltiplica superficie di sicurezza e manutenzione senza un bisogno
  chiaro.
- **Supporto Mosh:** utile in nicchia, ma aggiunge un secondo canale di trasporto da gestire.
- **Provider cloud/SSO dedicati:** meglio prima consolidare l'interoperabilità OpenSSH.

## Sequenza consigliata

1. 2FA/OTP e gestore known_hosts: blocco di sicurezza e compatibilità.
2. tmux/riconnessione: salto di qualità quotidiano.
3. Palette comandi (in corso), poi CRUD SFTP e snippet.
