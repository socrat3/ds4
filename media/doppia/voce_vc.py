# Voce per conversione: la voce originale diventa quella del campione, con gli stessi
# tempi, pause e intonazione (la bocca del video resta sincronizzata da sola). Per un
# video gia' nella lingua giusta: niente traduzione, niente sintesi.
#
# L'audio si converte a blocchi di frasi vicine (fino a ~30 s, tagliati nelle pause
# tra le frasi), ognuno rimesso al suo tempo in una traccia lunga quanto il video. I
# blocchi gia' convertiti restano in cartella: una ripresa non li rifa'.
#
# uso: voce_vc.py frasi.tsv voce_originale.wav campione.wav uscita.wav durata
import os, sys, time, warnings
warnings.filterwarnings("ignore")
import numpy as np, soundfile as sf

tsv, originale, campione, uscita, durata = sys.argv[1:6]
durata = float(durata)
cart = os.path.join(os.path.dirname(os.path.abspath(uscita)), "voce")
os.makedirs(cart, exist_ok=True)
frasi = []
for riga in open(tsv, encoding="utf-8"):
    c = riga.rstrip("\n").split("\t")
    if len(c) >= 2:
        frasi.append((float(c[0]), float(c[1])))
blocchi = []
for da, a in frasi:
    if blocchi and a - blocchi[-1][0] <= 30 and da - blocchi[-1][1] < 1.0:
        blocchi[-1][1] = a
    else:
        blocchi.append([da, a])
audio, sr = sf.read(originale, dtype="float32", always_2d=False)
if audio.ndim > 1:
    audio = audio.mean(axis=1)
modello = None
t0 = time.time()
traccia = np.zeros(int((durata + 1) * sr), dtype="float32")
for i, (da, a) in enumerate(blocchi):
    s, e = max(0, int((da - 0.15) * sr)), min(len(audio), int((a + 0.15) * sr))
    dest = os.path.join(cart, f"vc{i:04d}.wav")
    if not os.path.exists(dest):
        if modello is None:
            from chatterbox.vc import ChatterboxVC
            modello = ChatterboxVC.from_pretrained("cuda")
            print(f"voce: conversione pronta in {time.time()-t0:.0f} s", flush=True)
        pezzo = dest + ".in.wav"
        sf.write(pezzo, audio[s:e], sr)
        w = modello.generate(audio=pezzo, target_voice_path=campione).squeeze(0).cpu().numpy().astype("float32")
        if modello.sr != sr:
            w = np.interp(np.arange(0, len(w), modello.sr / sr), np.arange(len(w)), w).astype("float32")
        sf.write(dest + ".tmp.wav", w, sr)
        os.replace(dest + ".tmp.wav", dest)
        os.remove(pezzo)
        if i % 10 == 0:
            print(f"voce: blocco {i+1}/{len(blocchi)}", flush=True)
    w, _ = sf.read(dest, dtype="float32")
    fine = min(s + len(w), len(traccia))
    traccia[s:fine] = w[: fine - s]
traccia = np.clip(traccia[: int(durata * sr)], -1, 1)
sf.write(uscita + ".tmp.wav", traccia, sr, subtype="PCM_16")
os.replace(uscita + ".tmp.wav", uscita)
print(f"voce: {len(blocchi)} blocchi convertiti", flush=True)
