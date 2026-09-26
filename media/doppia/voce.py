# Voce italiana del doppiaggio: ogni frase di frasi.tsv sintetizzata con la voce
# clonata dal campione, adattata al suo spazio e messa al suo tempo, in un'unica
# traccia lunga quanto il video. Le frasi gia' sintetizzate non si rifanno.
#
# Adattamento (come nel resto della pipeline): la frase deve occupare ~95% del suo
# spazio; si accelera fino a 1,3x o si rallenta fino a 0,9x, e sotto il 5% si lascia
# com'e'. Le frasi ancora troppo lunghe si segnalano: invadono l'inizio della dopo.
#
# uso: voce.py qwen|xtts frasi.tsv campione.wav campione.txt cartella uscita.wav durata ffmpeg
import os, subprocess, sys, time, warnings
warnings.filterwarnings("ignore")
import numpy as np
import soundfile as sf

motore, tsv, campione, testo_campione, cart, uscita, durata, ffmpeg = sys.argv[1:9]
durata = float(durata)
SR = 24000
os.makedirs(cart, exist_ok=True)
frasi = []
for riga in open(tsv, encoding="utf-8"):
    campi = riga.rstrip("\n").split("\t")
    if len(campi) >= 4 and campi[3].strip():
        frasi.append((len(frasi), float(campi[0]), float(campi[1]), campi[3].strip()))
    elif len(campi) >= 4:
        frasi.append((len(frasi), float(campi[0]), float(campi[1]), ""))

def motore_qwen():
    import torch
    from qwen_tts import Qwen3TTSModel
    m = Qwen3TTSModel.from_pretrained("Qwen/Qwen3-TTS-12Hz-1.7B-Base", device_map="cuda:0",
                                      dtype=torch.bfloat16, attn_implementation="sdpa")
    prompt = m.create_voice_clone_prompt(ref_audio=campione, ref_text=open(testo_campione, encoding="utf-8").read().strip())
    def sintesi(t):
        w, sr = m.generate_voice_clone(text=t, language="Italian", voice_clone_prompt=prompt)
        return np.asarray(w[0], dtype="float32"), sr
    return sintesi

def motore_xtts():
    os.environ.setdefault("COQUI_TOS_AGREED", "1")   # licenza CPML: solo uso non commerciale
    import torch, torchaudio
    def carica(path, *a, **k):
        # torchaudio >= 2.9 legge con torchcodec, che vuole le librerie di FFmpeg: per i wav basta soundfile
        d, sr = sf.read(path, always_2d=True, dtype="float32")
        return torch.from_numpy(d.T.copy()), sr
    torchaudio.load = carica
    from TTS.api import TTS
    m = TTS("tts_models/multilingual/multi-dataset/xtts_v2").to("cuda")
    def sintesi(t):
        return np.asarray(m.tts(text=t, speaker_wav=campione, language="it"), dtype="float32"), m.synthesizer.output_sample_rate
    return sintesi

def fattore(d, spazio):
    f = min(max(d / (spazio * 0.95), 0.9), 1.3)
    return 1.0 if abs(f - 1) < 0.05 else round(f, 4)

sintesi = None
t0 = time.time()
for i, da, a, it in frasi:
    dest = os.path.join(cart, f"f{i:04d}.wav")
    if not it or os.path.exists(dest):
        continue
    if sintesi is None:
        sintesi = motore_qwen() if motore == "qwen" else motore_xtts()
        print(f"voce: modello {motore} caricato in {time.time()-t0:.0f} s", flush=True)
    w, sr = sintesi(it)
    if sr != SR:
        w = np.interp(np.arange(0, len(w), sr / SR), np.arange(len(w)), w).astype("float32")
    sf.write(dest + ".tmp.wav", w, SR)
    os.replace(dest + ".tmp.wav", dest)
    if i % 20 == 0:
        print(f"voce: frase {i+1}/{len(frasi)}", flush=True)

traccia = np.zeros(int((durata + 1) * SR), dtype="float32")
lunghe = rallentate = accelerate = 0
for i, da, a, it in frasi:
    src = os.path.join(cart, f"f{i:04d}.wav")
    if not it or not os.path.exists(src):
        continue
    w, _ = sf.read(src, dtype="float32")
    f = fattore(len(w) / SR, a - da)
    if f != 1.0:
        fit = os.path.join(cart, f"f{i:04d}_x{f}.wav")
        if not os.path.exists(fit):
            subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", src,
                            "-af", f"atempo={f}", "-ar", str(SR), "-ac", "1", fit], check=True)
        w, _ = sf.read(fit, dtype="float32")
        rallentate += f < 1
        accelerate += f > 1
    if len(w) / SR > (a - da) + 0.05:
        lunghe += 1
    s = int(da * SR)
    e = min(s + len(w), len(traccia))
    traccia[s:e] += w[: e - s]   # sommata: una frase lunga si sovrappone, non taglia la dopo
traccia = np.clip(traccia[: int(durata * SR)], -1, 1)
sf.write(uscita + ".tmp.wav", traccia, SR, subtype="PCM_16")
os.replace(uscita + ".tmp.wav", uscita)
print(f"voce: {len(frasi)} frasi, {rallentate} rallentate, {accelerate} accelerate, {lunghe} ancora lunghe", flush=True)
