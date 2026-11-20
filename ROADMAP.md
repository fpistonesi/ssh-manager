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

### 4. Gestore di known_hosts e verifica delle chiavi host — COMPLETATO
Finestra con host, fingerprint SHA-256, data di primo contatto; rimozione o aggiornamento di
una chiave; verifica esplicita al primo collegamento; messaggio leggibile quando la chiave
cambia.

**Perché:** oggi si usa `StrictHostKeyChecking=accept-new` e non c'è modo di ispezionare o
correggere ciò che è stato accettato. In caso di mismatch l'utente vede solo l'errore di
`ssh`. È un buco di sicurezza e un problema di assistenza molto comune.

**Sforzo:** S/M.

Stato: `StrictHostKeyChecking=ask`; l'helper askpass riconosce la richiesta di conferma e mostra
host, tipo e fingerprint SHA-256 (Annulla come scelta predefinita), anche per jump host, tunnel
in background e trasferimenti. Gestore "Chiavi host" con fingerprint, data di primo contatto
(registrata in `known_hosts.meta.json`), connessioni che usano la chiave, filtro, rimozione,
voci con nome cifrato (HashKnownHosts) e importazione da `~/.ssh/known_hosts`. Su chiave
cambiata, sessioni e tunnel mostrano un avviso con il nuovo fingerprint e "Rimuovi la chiave
salvata e riconnetti". Le chiavi accettate prima di questa versione non hanno data.

### 5. Snippet e cronologia comandi per host — COMPLETATO
Libreria di comandi con variabili (`${host}`, `${user}`, `${path}`), invio alla sessione
attiva o a tutte quelle selezionate; cronologia dei comandi digitati per host con “riesegui”;
comando di avvio automatico per connessione.

**Perché:** si integra con il broadcast esistente e trasforma un gestore di sessioni in uno
strumento operativo. Riduce errori su comandi ripetuti.

**Sforzo:** S/M.

Stato: pannello Snippet (nel vault) con `${host}`, `${user}`, `${port}`, `${name}` e variabili
libere chieste all'invio; invio alla sessione attiva o alle sessioni incluse nella
trasmissione, con o senza Invio. Pannello Cronologia per connessione (ultimi 200 comandi, nel
vault): la riga viene letta dallo schermo all'Invio, quindi le password non mostrate non
vengono mai registrate; esclusi i programmi a schermo intero, le righe senza prompt e i
comandi preceduti da uno spazio. Comando all'avvio per connessione (`ssh -t host 'cmd; exec
$SHELL -l'`). Resta: snippet nella palette Ctrl+K.

### 6. Trasferimento file completo, non solo copia — COMPLETATO
Crea cartella, rinomina, elimina, permessi; drag & drop tra i due pannelli; ripresa/retry
dopo interruzione; coda con trasferimenti paralleli; modifica di un file remoto con editor
locale e upload al salvataggio; segnalibri.

**Perché:** la finestra attuale sa navigare e copiare, ma per gestire file remoti serve ancora
il terminale. Le operazioni CRUD e il drag & drop sono ciò che la rende uno strumento
quotidiano.

**Sforzo:** M/L.

Stato: finestra non modale con una connessione per la navigazione e una per ogni trasferimento
parallelo (1–4); coda con avanzamento per elemento, annulla/riprova/rimuovi; lettura e scrittura
in pipeline (16 richieste da 64 KB in volo); download su `.part` e ripresa da dove si erano
fermati, riprova automatica (3 tentativi) sulle cadute di connessione. Nuova cartella, rinomina,
elimina (ricorsivo, senza seguire i link; in locale nel cestino) e permessi con dialogo
rwx/ottale; drag & drop tra i pannelli e da Esplora file; modifica remota con il programma
associato e invio a ogni salvataggio; segnalibri locali e remoti (questi nel vault).
Resta: conferma di sovrascrittura dei file già esistenti.

## Priorità media

### 7. Tunnel completi e gestiti come servizi — IN CORSO
`-R` remoto e `-D` SOCKS dinamico; stato per tunnel (attivo/errore), riavvio automatico,
avvio senza aprire un terminale, indicatore nella tray.

**Perché:** oggi esistono solo tunnel locali `-L` creati all'avvio della sessione. Chi
sviluppa usa continuamente SOCKS e reverse tunnel.

**Sforzo:** M.

Stato: tipi `-L`/`-R`/`-D` per tunnel (archivi esistenti letti come `-L`); pannello Tunnel con
i tunnel delle sessioni e dei servizi; "Avvia tunnel" esegue `ssh -N -o ExitOnForwardFailure=yes`
senza terminale, con stato in avvio/attivo/riconnessione/errore ricavato dall'output di `ssh -v`
e dalla tabella dei socket, riavvio automatico con attesa crescente (5–60 s) e stop sugli errori
di autenticazione o di chiave host. I servizi restano attivi con l'archivio bloccato.
Icona nella tray con lo stato complessivo (verde/arancio/rosso), menu con i tunnel
(riavvia/ferma), notifiche di interruzione, errore e ripristino; chiudendo la finestra con
tunnel attivi l'app resta nella tray (disattivabile), Esci la chiude davvero.
Resta: avvio automatico dei servizi all'apertura dell'app.

### 8. Registrazione e audit delle sessioni
Log in formato asciicast (o testo con timestamp), cifrati nel vault o su file con policy per
host; replay; redazione di pattern sensibili; esportazione.

**Perché:** serve per diagnosi, passaggio di consegne e compliance; si sposa con l'architettura
del vault.

**Sforzo:** M.

### 9. Interoperabilità OpenSSH: import `~/.ssh/config`, chiavi, `ssh-copy-id` — COMPLETATO
Import di `Host`, `HostName`, `User`, `Port`, `ProxyJump`, `IdentityFile`; generazione chiavi
ed25519; pulsante “copia chiave pubblica sull'host”; export verso config OpenSSH.

**Perché:** oggi l'inserimento è manuale e chi ha già un `~/.ssh/config` deve ricopiare tutto.
L'import abbatte la barriera d'ingresso.

**Sforzo:** S/M.

Stato: Archivio > OpenSSH. Import di `~/.ssh/config` con la stessa risoluzione di ssh (primo
valore vince, `Host *` e jolly come valori predefiniti, `Include`), IdentityFile con `~`/`%d`/
`%h`/`%r`, ProxyJump (anche a più salti, i salti letterali diventano connessioni),
Local/Remote/DynamicForward come tunnel; scelta di cosa importare e dove con lo stesso dialogo
dell'import selettivo, e host già presenti (stessi host, utente, porta) aggiornati, saltati o
copiati; avvisi per ProxyCommand e catene non rappresentabili. Export in blocchi `Host` (con
ProxyJump e forward) da copiare o salvare. "Genera chiave SSH" (ed25519 o RSA 4096 con
ssh-keygen, passphrase facoltativa) che può impostarla sulla connessione e installarla
sull'host; "Copia la chiave pubblica sull'host" come ssh-copy-id: la chiave passa su stdin a uno
script `sh` che non la duplica in `authorized_keys`, poi propone di accedere con quella chiave.
`Match` non viene valutato.

### 10. Fine sessione e notifiche ben gestite — COMPLETATO
Exit code e motivo della disconnessione, pulsante “Riconnetti”, notifica desktop per sessioni
lunghe o tunnel caduti, indicatore di raggiungibilità host nell'albero.

**Perché:** oggi la sessione finita mostra solo “sessione terminata”. Sapere perché è finita e
poter ripartire con un clic evita di accorgersi tardi di un tunnel morto.

**Sforzo:** S/M.

Stato: barra di fine sessione con motivo leggibile (dai messaggi di ssh), riga originale, codice
di uscita e durata, pulsanti Riconnetti (stessa posizione, credenziali rilette dal vault) e
Chiudi; la finestra "exited unexpectedly" di qtermwidget è disattivata (patch 0003). Notifica
desktop quando una sessione cade e non è in primo piano; opzione per sessione "Avvisami quando
l'output si ferma" per i comandi lunghi; i tunnel caduti erano già notificati (punto 7).
Raggiungibilità: pallino verde/rosso nell'albero con latenza e versione del server, verifica
manuale per host e verifica periodica (5 min) disattivata per default perché finisce nei log del
server; gli host dietro jump host non vengono verificati.

### 11. Esecuzione batch su host selezionati — COMPLETATO
Selezione multipla nell'albero, comando da eseguire su tutti, output aggregato per host con
exit code, salvataggio del risultato.

**Perché:** il broadcast è interattivo e non permette di lanciare un comando su un sottoinsieme
raccogliendone l'esito.

**Sforzo:** M.

Stato: selezione multipla nell'albero (una cartella vale per tutti i suoi host); finestra con
elenco di host spuntabili, comando o snippet con variabili per host, parallelismo e timeout;
per ogni host stato, codice di uscita, durata, stdout e stderr; riesegui sui non riusciti,
salvataggio in testo o JSON. Esecuzione `ssh -T` con la stessa autenticazione delle sessioni.

### 12. Dashboard di salute dell'host — COMPLETATO
Pannello con load, memoria, disco, uptime, servizi systemd falliti e processi principali, letto
via una singola esecuzione SSH o riusando la sessione.

**Perché:** evita di aprire cinque terminali per capire cosa sta succedendo, senza agent remoti.

**Sforzo:** M.

Stato: "Stato dell'host" esegue uno script POSIX in sola lettura con `sh -s` (una sola
connessione) e mostra sistema, uptime, carico rispetto ai core, memoria e swap, dischi con uso
colorato, unità systemd fallite e processi più attivi; aggiornamento manuale o ogni 30 s. Le
sezioni il cui strumento manca sul server restano vuote senza errori.

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

### 15. Internazionalizzazione e accessibilità dell'app principale — IN CORSO
Risorse IT/EN, navigazione completa da tastiera, nomi accessibili e contrasto adeguato.

**Perché:** intervento relativamente economico che allarga il pubblico e migliora la qualità
percepita.

**Sforzo:** S/M.

Stato: traduzioni IT/EN (`translations/*.ts`, compilate nelle risorse) e traduzioni di Qt per i
dialoghi standard; lingua scelta da Visualizza > Aspetto > Lingua (sistema/italiano/inglese),
applicata al riavvio. Tema chiaro/scuro con contrasto curato (vedi "Fatto fuori roadmap").
Resta: navigazione completa da tastiera, nomi accessibili, cambio lingua senza riavvio.

### 16. Packaging, CI e test automatici — IN PARTE
PKGBUILD per Arch/Manjaro, AppImage o Flatpak, GitHub Actions con build headless, test del
vault e del parser SFTP, ASan/UBSan.

**Perché:** il codice contiene crypto e un parser di protocollo scritto a mano: sono le parti
in cui una regressione silenziosa costa cara.

**Sforzo:** M.

Stato: packaging Windows con `packaging/windows/package.ps1` (build Release, `windeployqt`,
licenze, verifica delle DLL e dell'avvio dal pacchetto, zip portabile, installer Inno Setup
facoltativo) e icona/versione nell'eseguibile. Restano i pacchetti Linux, la CI e i test
automatici.

### 17. Rifinitura del terminale — COMPLETATO
Ricerca nello scrollback (da verificare rispetto a quanto già offre QTermWidget), link/IP
cliccabili, copia alla selezione, indicatore di attività/bell per sessione, tema chiaro
coerente.

**Perché:** sono piccoli attriti quotidiani; insieme fanno sembrare l'app molto più curata.

**Sforzo:** S/M.

Stato: ricerca con la barra di QTermWidget (Ctrl+Shift+F o menu); link aperti con Ctrl+clic e
da "Apri link" (prima non succedeva nulla); un host o IP selezionato offre "Connessione rapida
a…"; copia alla selezione come opzione; pallino blu (nuovo output) o arancio (campanello) nel
pannello Sessioni e nel titolo di finestra/scheda per le sessioni in secondo piano, con
lampeggio nella barra delle applicazioni al campanello; tema Sistema/Chiaro/Scuro
(Visualizza > Aspetto) applicato dal vivo, e schema terminale "Automatico" (predefinito per le
nuove connessioni) che segue il tema.

### 18. Backup versionato, sincronizzazione e igiene del vault
Backup cifrati automatici con rotazione, sincronizzazione opzionale via cartella/cloud
dell'utente, merge in importazione; audit di password/chiavi vecchie o duplicate, fingerprint
delle chiavi.

**Perché:** la perdita o la corruzione del vault significa perdere tutte le credenziali; oggi
l'export è manuale.

**Sforzo:** M.

### 19. Esportazione e importazione selettiva delle connessioni — COMPLETATO
Scegliere quali connessioni/cartelle esportare in un file cifrato da passare a un collega, con o
senza credenziali; in importazione scegliere cosa aggiungere, dove, e come trattare le
connessioni già presenti, invece di sostituire l'intero archivio.

**Perché:** oggi export e import riguardano tutto l'archivio, credenziali comprese: per
condividere qualche host con un altro tecnico bisogna dargli tutto o ricopiare a mano.

**Sforzo:** S/M.

## Fatto fuori roadmap

- **Port Windows** (prima tra le idee sconsigliate): build MinGW, QTermWidget con ConPTY
  (patch in `patches/qtermwidget`), askpass e OpenSSH di Windows.
- **Interfaccia:** ribbon a schede con gruppi compatti; vista a schede oltre a quella MDI;
  pannelli agganciabili con posizione/dimensione salvate e "Ripristina layout"; icona nella
  tray; tema Sistema/Chiaro/Scuro in stile Microsoft 365 (Fusion + QSS, cambio dal vivo); tema e
  lingua in menu a tendina.
- **Albero connessioni:** indicatore filtri attivi; colonne ridimensionabili e colonne opzionali
  Destinazione/Preferito (★)/Tag; ordinamento manuale con trascinamento (anche tra cartelle) o
  alfabetico naturale, e riordino alfabetico permanente dell'archivio; nuova cartella al livello
  principale cliccando nell'area vuota.
- **Terminale:** copia/incolla da menu e scorciatoie, "Pulisci terminale" funzionante su Windows.
- **Modalità portabile:** un `vault.db` accanto all'eseguibile ha la priorità; archivio,
  `known_hosts` e impostazioni (file `.ini`) restano nella cartella dell'app, i file temporanei
  delle sessioni sul PC locale. Un `vault.db` vuoto avvia la creazione di un nuovo archivio.
- **Jump host:** flag "Usa come jump host" per connessione; "Passa per" elenca solo quelle,
  in ordine alfabetico.
- **Strumenti esterni:** programmi lanciati su una connessione (ping, tracert, test porta, ftp,
  browser...) con variabili `${host}`/`${port}`/`${user}`/`${name}`, in una scheda terminale, in
  una finestra propria o come URL; salvati nelle impostazioni, non nel vault; dal menu del tasto
  destro e dalla palette.

## Idee che sconsiglio di affrontare ora

- **Plugin/scripting API:** moltiplica superficie di sicurezza e manutenzione senza un bisogno
  chiaro.
- **Supporto Mosh:** utile in nicchia, ma aggiunge un secondo canale di trasporto da gestire.
- **Provider cloud/SSO dedicati:** meglio prima consolidare l'interoperabilità OpenSSH.

## Sequenza consigliata

Completati: 3, 4, 5, 6, 9, 10, 11, 12, 17, 19. Prossimi passi:

1. 2FA/OTP (2), subito se ci sono host che lo richiedono.
2. tmux/riconnessione (1): salto di qualità quotidiano.
3. Backup automatici (18) e avvio automatico dei tunnel (resto del 7).
