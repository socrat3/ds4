# Disegna il titolo di testa come PNG WxH dal file dei titoli del lavoro.
# Il file e' dell'utente: righe "chiave: testo", '#' commenta. Le righe di testo sono
# disegnate nell'ordine in cui compaiono, e la chiave ne sceglie lo stile:
#   grande:  grassetto grande     testo:  normale     piccolo:  piccolo
#   spazio:  una riga vuota (il testo dopo i due punti e' ignorato)
# mostra e durata non si disegnano (le legge titolo.sh).
# uso: titolo.py W H uscita.png titoli.txt
import sys
from PIL import Image, ImageDraw, ImageFont

def leggi_righe(path):
    righe = []
    for r in open(path, encoding="utf-8"):
        r = r.rstrip("\n")
        if not r.strip() or r.lstrip().startswith("#") or ":" not in r: continue
        k, v = r.split(":", 1)
        righe.append((k.strip().lower(), v.strip()))
    return righe

W, H = int(sys.argv[1]), int(sys.argv[2])
out, righe = sys.argv[3], leggi_righe(sys.argv[4])
S = min(W, H)
REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
STILI = {"grande": (BOLD, S // 18, (255, 255, 255), 0.84),
         "testo": (REG, S // 26, (216, 221, 227), 0.84),
         "piccolo": (REG, S // 34, (170, 178, 187), 0.74)}
img = Image.new("RGB", (W, H), (20, 24, 29))
d = ImageDraw.Draw(img)

def a_capo(testo, f, larghezza):
    # a capo sulla larghezza vera del testo, non sul numero di caratteri
    out, riga = [], ""
    for p in testo.split():
        prova = (riga + " " + p).strip()
        if d.textlength(prova, font=f) <= larghezza or not riga: riga = prova
        else: out.append(riga); riga = p
    if riga: out.append(riga)
    return out

blocchi = []   # (font, righe, passo, colore)
for k, v in righe:
    if k == "spazio":
        blocchi.append((None, [""], S // 26, None))
    elif k in STILI and v:
        font_path, size, colore, larg = STILI[k]
        f = ImageFont.truetype(font_path, size)
        blocchi.append((f, a_capo(v, f, W * larg), int(size * 1.4), colore))
y = (H - sum(len(b[1]) * b[2] for b in blocchi)) // 2   # tutto centrato in verticale
for f, rr, passo, colore in blocchi:
    for r in rr:
        if f: d.text(((W - d.textlength(r, font=f)) / 2, y), r, font=f, fill=colore)
        y += passo
img.save(out)
