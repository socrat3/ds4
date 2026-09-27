/* ds4_media_help - l'aiuto di ds4-media: generale e per comando.
 *
 *   ds4-media help [COMANDO]     ds4-media --help     ds4-media COMANDO --help
 *
 * Il testo sta qui, in un posto solo, e dice per ogni opzione il valore predefinito e
 * i limiti che il codice applica davvero. Le voci di configurazione di `doppia` non
 * sono ricopiate: si stampano dalla tabella DOPPIA_VOCI, quindi una voce nuova compare
 * nell'aiuto senza toccare questo file. */
#include "ds4_media_help.h"
#include "ds4_media.h"
#include "ds4_media_doppia.h"

#include <string.h>

static void generale(FILE *o) {
    fputs(
"ds4-media - immagini, video e doppiaggi generati in locale con ComfyUI\n"
"            (Qwen-Image 2.1 per le immagini, MiniMax H3 per i video).\n"
"\n"
"uso: ds4-media COMANDO [opzioni] [--] [parole...]\n"
"\n"
"Comandi\n"
"  img      genera un'immagine da una descrizione, o ne modifica una (--rif)\n"
"  video    anima un'immagine: video con audio dal primo fotogramma (H3)\n"
"  doppia   doppia un video in italiano con il tuo volto e la tua voce\n"
"  serve    server compatibile con l'API Immagini di OpenAI (per Open WebUI)\n"
"  clean    cancella le immagini, i video e i log generati\n"
"  health   dice se ComfyUI risponde\n"
"  free     fa scaricare i modelli a ComfyUI e restituisce la memoria\n"
"  help     questo aiuto; \"ds4-media help COMANDO\" per i dettagli\n"
"\n"
"Opzioni valide per tutti i comandi\n"
"  --comfy-port N   porta di ComfyUI                        (predefinita 8188)\n"
"  --host H         indirizzo di ComfyUI                    (predefinito 127.0.0.1)\n"
"  --dir D          cartella di uscita                      (predefinita ~/.ds4/media;\n"
"                   per doppia: la cartella del lavoro)\n"
"  --bf16           pesi bf16 invece di int8: piu' fedeli, ~2x la memoria\n"
"  --no-gate        salta il controllo della memoria prima del lavoro\n"
"  --tieni-comfy    non cancellare la copia che ComfyUI lascia in ComfyUI/output\n"
"  -h, --help       aiuto del comando\n"
"\n"
"Come si leggono gli argomenti\n"
"  Le opzioni vanno in qualsiasi posizione; tutto cio' che non e' un'opzione e'\n"
"  la descrizione (le parole si uniscono con uno spazio). \"--\" chiude le opzioni:\n"
"  quello che segue e' descrizione anche se comincia con '-'. I numeri sono\n"
"  controllati per intero: \"--seed 12x\" o \"--cfg 0x10\" sono errori.\n"
"  Attenzione: dopo \"--\" anche --comfy-port e --dir diventano parole.\n"
"\n"
"Ctrl+C\n"
"  Il primo annulla il lavoro in corso: lo toglie dalla coda di ComfyUI e lo\n"
"  interrompe, cosi' la GPU non resta occupata. Il secondo esce subito.\n"
"\n"
"Codici di uscita\n"
"  0 riuscito   1 errore (ComfyUI, rete, disco)   2 uso sbagliato o richiesta\n"
"  non valida   130 annullato con Ctrl+C\n"
"\n"
"File\n"
"  ~/.ds4/media/            immagini e video (img-*.png, vid-*.mp4)\n"
"  ~/.ds4/doppia.conf       le tue scelte stabili per doppia (foto, voce, strumenti)\n"
"  ~/.ds4/doppia/LAVORO/    un doppiaggio: file di ogni fase, log, video finali\n"
"\n"
"Esempi\n"
"  ds4-media img un faro al tramonto, stile acquerello\n"
"  ds4-media video --rif faro.png --sec 8 le onde si infrangono sugli scogli\n"
"  ds4-media doppia https://www.youtube.com/watch?v=ID --da 0 --a 60\n"
"  ds4-media help doppia\n", o);
}

static void img(FILE *o) {
    fputs(
"ds4-media img - un'immagine da una descrizione (Qwen-Image 2.1 via ComfyUI)\n"
"\n"
"uso: ds4-media img [opzioni] [--] descrizione...\n"
"\n"
"Opzioni\n"
"  --size WxH       dimensioni in pixel: multipli di 32, lato <= 2752,\n"
"                   area <= 2752x1536 (4,2 megapixel)      (predefinita 1024x1024;\n"
"                   con --rif e senza --size: proporzioni e area del 1o riferimento)\n"
"  --n N            immagini nello stesso lavoro, 1..4, un solo encoding  (1)\n"
"  --seed S         seme; -1 = casuale                                   (-1)\n"
"  --passi N        passi di campionamento, 1..60                        (25)\n"
"  --cfg C          guida, 0.1..20                    (1; 2.5 con --negativo)\n"
"  --negativo T     cosa evitare; con cfg 1 ComfyUI lo ignora, per questo senza\n"
"                   --cfg il valore sale da solo a 2.5 (costa ~2x il tempo)\n"
"  --trasparente    sfondo trasparente: PNG con canale alfa\n"
"  --rif FILE       immagine di riferimento, fino a 10 (--rif A --rif B ...): la\n"
"                   prima e' quella da modificare, le altre fanno da riferimento\n"
"  --bf16           pesi bf16 (vedi memoria)\n"
"  --no-free        non proporre di liberare memoria prima del lavoro\n"
"  --auto-free      liberala senza chiedere se manca\n"
"  --no-vista       non proporre di aprire il risultato\n"
"\n"
"Memoria richiesta (compresi 6 GiB di margine)\n"
"  int8 ~24 GiB, bf16 ~38 GiB fino a 1 megapixel e ~43 GiB oltre, +2 GiB per\n"
"  megapixel per ogni immagine in piu' con --n. Conta come disponibile anche la\n"
"  memoria che ComfyUI tiene gia' (i pesi dell'immagine precedente).\n"
"\n"
"Uscita\n"
"  ~/.ds4/media/img-AAAAMMGG-HHMMSS-ID-NNN.png; a video dimensioni, seme e tempi\n"
"  per fase (coda, generazione, scarico). Il file non sovrascrive mai un altro.\n"
"\n"
"Esempi\n"
"  ds4-media img --size 1536x1024 --n 4 una piazza di Palermo di notte\n"
"  ds4-media img --rif foto.png aggiungi un cappello di paglia\n"
"  ds4-media img --negativo \"sfocato, testo\" --seed 7 -- -un titolo che inizia con il trattino\n", o);
}

static void video(FILE *o) {
    fputs(
"ds4-media video - anima un'immagine (MiniMax H3, video con audio)\n"
"\n"
"uso: ds4-media video --rif FILE [opzioni] [--] descrizione...\n"
"\n"
"Opzioni\n"
"  --rif FILE       il primo fotogramma (obbligatorio, uno solo)\n"
"  --sec S          durata in secondi, 0.1..15, portata sulla griglia di H3\n"
"                   (17k+5 fotogrammi a 24 fps)                           (5)\n"
"  --size WxH       multipli di 32, stessi limiti di img            (864x480)\n"
"  --passi N        1..60                                                (20)\n"
"  --seed S         -1 = casuale                                         (-1)\n"
"  --no-free / --auto-free / --no-vista   come in img\n"
"\n"
"Memoria\n"
"  H3 vuole la GPU quasi intera: ~100 GiB per ComfyUI. Spegni prima ogni altro\n"
"  modello (spark-switch), o accetta la proposta di liberare memoria. Dura minuti\n"
"  (~11 minuti per 15 s a 576x576 sul DGX Spark).\n"
"\n"
"Uscita\n"
"  ~/.ds4/media/vid-AAAAMMGG-HHMMSS-ID-000.mp4\n"
"\n"
"Esempio\n"
"  ds4-media video --rif ritratto.png --sec 10 sorride e saluta con la mano\n", o);
}

static void doppia(FILE *o) {
    fputs(
"ds4-media doppia - doppia un video in italiano con il tuo volto e la tua voce\n"
"\n"
"uso: ds4-media doppia URL|FILE [opzioni]\n"
"     FILE: qualsiasi video che ffmpeg legge (mp4, mkv, mov, webm, avi, ts, m4v...),\n"
"     con una traccia audio. Si copia se mp4 accetta i suoi flussi, se no si ricodifica.\n"
"     ds4-media doppia --riprendi CARTELLA_DEL_LAVORO [opzioni]\n"
"\n"
"Opzioni (vincono sul doppia.conf del lavoro, che vince su ~/.ds4/doppia.conf)\n"
"  --da S           secondo di inizio nel video originale               (0)\n"
"  --a S            secondo di fine; 0 = fino alla fine                 (0)\n"
"  --resa R         testa | scena | entrambe                     (entrambe)\n"
"  --voce M         qwen (Qwen3-TTS) | xtts (XTTS-v2)                 (qwen)\n"
"  --senza-titolo   nessuna schermata iniziale\n"
"  --dir D          cartella del lavoro       (~/.ds4/doppia/ID[_da-a])\n"
"  --riprendi D     riprende il lavoro nella cartella D\n"
"  --si             niente wizard: parte con le scelte gia' salvate\n"
"\n"
"Il wizard\n"
"  Se sei al terminale (e senza --si), prima di partire mostra tutte le scelte\n"
"  numerate. Scrivi il numero di una voce per cambiarla (Invio tiene il valore,\n"
"  '-' la svuota), t per aprire le righe del titolo nell'editor ($EDITOR, o nano),\n"
"  p per salvare le scelte come predefinite in ~/.ds4/doppia.conf, Invio per\n"
"  partire, q per uscire senza salvare. Non parte se mancano video, foto,\n"
"  campione di voce (e il suo testo, per qwen) o il modello di whisper.\n"
"\n"
"Le fasi (ognuna ha il suo file: se c'e', la fase si salta)\n"
"   1 sorgente    sorgente.mp4           yt-dlp o il file locale, poi taglio --da/--a\n"
"   2 lingua      lingua.txt             riconosciuta dall'audio (lingua = auto)\n"
"   3 trascrivi   parole.csv             whisper.cpp, una parola per riga, con il\n"
"                                        glossario come suggerimento\n"
"   4 frasi       frasi.tsv              frasi da 3 a 9 s\n"
"   5 traduci     frasi.tsv (4a colonna) ds4-server, blocchi da 20 frasi; saltata se\n"
"                                        il video e' gia' in italiano\n"
"   6 voce        voce_it.wav            gia' italiano: la voce originale convertita\n"
"                                        nella tua (stessi tempi, labiale intatto);\n"
"                                        tradotto: la traduzione letta con la tua voce\n"
"   7 inquadrature posizione.txt         tratto per tratto: webcam tonda, webcam\n"
"                                        rettangolare, persona a tutto schermo, nessuno\n"
"   8 pezzi       pezzi/pNNN.mp4         teste parlanti H3 da 15 s, ancorate alla\n"
"                                        foto all'inizio e alla fine\n"
"   9 monta       testa.mp4, scena.mp4   testa messa nel posto di ogni tratto (niente\n"
"                                        nei tratti senza persona), voce a -16 LUFS\n"
"  10 titolo      LAVORO_testa.mp4, LAVORO_scena.mp4   (solo se cambiato)\n"
"  posizione.txt si puo' correggere a mano: una riga per tratto,\n"
"    DA A cerchio CX CY R | DA A riquadro X Y W H | DA A intero | DA A vuoto\n"
"  (poi cancella scena.mp4 e riprendi).\n"
"  Per rifare una fase cancella il suo file (e quelli delle fasi dopo). Ogni\n"
"  programma esterno scrive in doppia.log.\n"
"\n"
"Motori\n"
"  Il traduttore (~80 GiB) e ComfyUI con H3 (~110 GiB) non stanno insieme. Con\n"
"  cambio_motori = si, doppia li accende da solo con spark-switch quando servono\n"
"  (fermando il motore acceso); con no, dice quale riga accendere e si ferma.\n"
"\n"
"Il titolo di testa: titoli.txt nella cartella del lavoro\n"
"  Nasce dai dati di YouTube e poi e' tuo: non viene mai sovrascritto.\n"
"    mostra: si|no     durata: secondi\n"
"    grande: TESTO     testo: TESTO     piccolo: TESTO     spazio:\n"
"  Le righe si disegnano nell'ordine, centrate. Dopo averlo cambiato, rilancia\n"
"  con --riprendi: si rifa' solo il titolo, in pochi secondi.\n"
"\n"
"Tempi sul DGX Spark\n"
"  ~11 minuti di GPU per ogni pezzo da 15 s: un'ora di video = ~240 pezzi, ~2 giorni.\n"
"  Ctrl+C ferma pulito; lo stesso comando (o --riprendi) riparte dall'ultimo pezzo.\n"
"\n"
"Esempi\n"
"  ds4-media doppia https://www.youtube.com/watch?v=ID --da 60 --a 120\n"
"  ds4-media doppia lezione.mp4 --resa testa --voce xtts --si\n"
"  ds4-media doppia --riprendi ~/.ds4/doppia/ID\n"
"\n"
"Voci di configurazione (doppia.conf: chiave = valore; tutte cambiabili nel wizard)\n", o);
    const char *sez = "";
    for (int i = 0; i < DOPPIA_N_VOCI; i++) {
        const doppia_voce *d = &DOPPIA_VOCI[i];
        if (strcmp(sez, d->sezione)) { fprintf(o, "  %s\n", d->sezione); sez = d->sezione; }
        fprintf(o, "    %-19s %s", d->chiave, d->etichetta);
        if (d->aiuto[0]) fprintf(o, ": %s", d->aiuto);
        if (d->scelte) fprintf(o, " [%s]", d->scelte);
        if (d->predefinito[0]) fprintf(o, " (%s)", d->predefinito);
        fputc('\n', o);
    }
}

static void serve(FILE *o) {
    fputs(
"ds4-media serve - server compatibile con l'API Immagini di OpenAI\n"
"\n"
"uso: ds4-media serve [--port N] [--idle-free S] [opzioni comuni]\n"
"\n"
"Opzioni\n"
"  --port N         porta del server, solo su 127.0.0.1               (8010)\n"
"  --idle-free S    dopo S secondi senza lavori scarica i modelli da ComfyUI;\n"
"                   0 = mai                                              (0)\n"
"  --bf16           pesi bf16 per le richieste che non indicano \"weights\"\n"
"\n"
"Rotte\n"
"  POST /v1/images/generations   JSON: prompt (obbligatorio), negative_prompt,\n"
"        n (1..4, un solo lavoro), size (WxH o auto), response_format (url |\n"
"        b64_json), seed, steps, weights (int8 | bf16). 400 per le richieste non\n"
"        valide, 502 se ComfyUI fallisce.\n"
"  GET  /v1/media/files/NOME     il file di una risposta con response_format=url\n"
"  GET  /v1/models               elenca qwen-image-2.1\n"
"  GET  /health                  200 se ComfyUI risponde\n"
"  Un client che chiude la connessione annulla il suo lavoro. Limiti: 64\n"
"  connessioni, header 64 KiB, corpo 1 MiB, 30 s di silenzio per richiesta.\n"
"\n"
"Open WebUI\n"
"  Impostazioni > Immagini: motore OpenAI, URL http://127.0.0.1:8010/v1, una\n"
"  chiave qualsiasi, modello qwen-image-2.1.\n"
"\n"
"Esempio\n"
"  curl -s http://127.0.0.1:8010/v1/images/generations \\\n"
"       -d '{\"prompt\":\"un gatto rosso\",\"size\":\"1024x1024\",\"response_format\":\"url\"}'\n", o);
}

static void clean(FILE *o) {
    fputs(
"ds4-media clean - cancella quello che ds4-media ha generato\n"
"\n"
"uso: ds4-media clean [--dir D] [--comfy] [--no-log] [--forza]\n"
"\n"
"  Mostra quanti file e quanti MB cancellerebbe, poi chiede conferma. Tocca solo\n"
"  img-*.png, vid-*.mp4 e *.log della cartella: mai sottocartelle, link o altro.\n"
"  --dir D          cartella da pulire                   (~/.ds4/media)\n"
"  --comfy, --tutto anche le copie in ~/comfy/ComfyUI/output/ds4\n"
"  --no-log         lascia i *.log\n"
"  --forza, -y      non chiedere conferma\n"
"  Non tocca i lavori di doppia (~/.ds4/doppia).\n", o);
}

static void semplice(FILE *o, const char *cmd) {
    if (!strcmp(cmd, "health"))
        fputs("ds4-media health - stampa \"ok\" se ComfyUI risponde su /system_stats, altrimenti\n"
              "il motivo (esce con 1). Opzioni: --comfy-port, --host.\n", o);
    else
        fputs("ds4-media free - chiede a ComfyUI di scaricare i modelli e liberare la memoria\n"
              "(POST /free), per restituirla a un modello di chat. Opzioni: --comfy-port, --host.\n", o);
}

int ds4_media_help(FILE *o, const char *cmd) {
    if (!cmd || !cmd[0] || !strcmp(cmd, "help")) generale(o);
    else if (!strcmp(cmd, "img")) img(o);
    else if (!strcmp(cmd, "video")) video(o);
    else if (!strcmp(cmd, "doppia")) doppia(o);
    else if (!strcmp(cmd, "serve")) serve(o);
    else if (!strcmp(cmd, "clean")) clean(o);
    else if (!strcmp(cmd, "health") || !strcmp(cmd, "free")) semplice(o, cmd);
    else {
        fprintf(stderr, "ds4-media: nessun aiuto per \"%s\"; comandi: img video doppia serve clean health free\n", cmd);
        return 2;
    }
    return 0;
}
