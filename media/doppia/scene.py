# La mappa delle inquadrature di un video, tratto per tratto: dove sta chi parla.
#
#   da a cerchio CX CY R      webcam tonda sopra uno schermo registrato
#   da a riquadro X Y W H     webcam rettangolare (o un riquadro qualsiasi con la persona)
#   da a intero               la persona in scena, grande
#   da a vuoto                nessuna persona (o una transizione troppo breve)
#
# Una passata sola sul video, solo CPU. Ogni fotogramma: differenza con il precedente
# (64x36 in grigi) per i tagli d'inquadratura. Ogni 6 fotogrammi: volti con YuNet a
# meta' risoluzione, e un fotogramma piccolo in grigi per la mediana dell'inquadratura.
# A fine inquadratura si decide il tipo: volto grande = intero; volto piccolo = si cerca
# il suo contenitore sulla mediana (dove lo schermo registrato si media via e la cornice
# della webcam resta): prima un cerchio (Hough), poi un rettangolo cercato come quattro
# linee dritte intorno al volto; se non c'e' cornice, un riquadro intorno al volto. Un
# volto piccolo che si sposta (persone in una foto) non e' una webcam: tratto vuoto.
# Tratti vicini con la stessa geometria si uniscono.
#
# uso: scene.py video uscita.txt
import os, sys
import cv2, numpy as np

video, uscita = sys.argv[1:3]
modello = "face_detection_yunet_2023mar.onnx"
for d in (os.path.expanduser("~/.ds4"), os.path.dirname(os.path.abspath(__file__))):
    if os.path.exists(os.path.join(d, modello)):
        modello = os.path.join(d, modello)
        break
if not os.path.exists(modello):
    sys.exit("manca face_detection_yunet_2023mar.onnx (in ~/.ds4): opencv_zoo, licenza MIT")

cap = cv2.VideoCapture(video)
fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
W = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)); H = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
if W <= 0 or H <= 0:
    sys.exit("non leggo il video")
S = 2                                    # volti a meta' risoluzione
volti = cv2.FaceDetectorYN.create(modello, "", (W // S, H // S), 0.75, 0.3, 5000)
M = 4                                    # mediana a un quarto
PASSO, SOGLIA_TAGLIO, MIN_TRATTO = 6, 12.0, 0.5

def cornice(med, fx, fy, fh):
    """Il riquadro intorno al volto (coordinate della mediana): partendo dal volto, la
    prima linea dritta e lunga a sinistra, a destra, sopra e sotto. I bordi si cercano
    sui tre canali di colore: il verde di una webcam e il viola di un desktop hanno la
    stessa luminosita' e in grigi il bordo sparisce. Funziona anche con gli angoli
    arrotondati, che i contorni spezzavano. None se manca un lato."""
    e = np.zeros(med.shape[:2], np.uint8)
    for ch in cv2.split(cv2.medianBlur(med, 3)):
        e |= cv2.Canny(ch, 20, 60)
    e = e > 0
    h, w = e.shape
    y0, y1 = int(max(0, fy - fh)), int(min(h, fy + fh))
    x0, x1 = int(max(0, fx - fh)), int(min(w, fx + fh))
    def cerca(valori, colonna):
        for v in valori:
            if (e[y0:y1, v].mean() if colonna else e[v, x0:x1].mean()) > 0.55:
                return v
        return None
    L = cerca(range(int(fx - 0.8 * fh), max(-1, int(fx - 4 * fh)), -1), True)
    R = cerca(range(int(fx + 0.8 * fh), min(w, int(fx + 4 * fh))), True)
    T = cerca(range(int(fy - 0.8 * fh), max(-1, int(fy - 4 * fh)), -1), False)
    B = cerca(range(int(fy + 0.8 * fh), min(h, int(fy + 4 * fh))), False)
    if None in (L, R, T, B):
        return None
    return L, T, R - L, B - T

def classifica(da, a, facce, piccoli):
    """Il tipo dell'inquadratura [da, a) dai volti campionati e dalle immagini piccole."""
    if a - da < MIN_TRATTO or not facce:
        return ("vuoto",)
    con = [f for f in facce if f is not None]
    if len(con) < 0.4 * len(facce):
        return ("vuoto",)
    hmed = float(np.median([f[3] for f in con]))
    if hmed >= 0.2 * H:
        return ("intero",)
    cxs = [f[0] + f[2] / 2 for f in con]; cys = [f[1] + f[3] / 2 for f in con]
    cx, cy = float(np.median(cxs)), float(np.median(cys))
    # una webcam sta ferma: volti piccoli che saltano (persone in una foto, un pubblico)
    # non sono chi parla, e il tratto resta com'e'
    if float(np.std(cxs)) > 0.5 * hmed or float(np.std(cys)) > 0.5 * hmed:
        return ("vuoto",)
    med = np.median(np.stack(piccoli), axis=0).astype("uint8") if piccoli else None
    if med is not None:
        g = cv2.medianBlur(cv2.cvtColor(med, cv2.COLOR_BGR2GRAY), 3)
        h4 = H // M
        cerchi = cv2.HoughCircles(g, cv2.HOUGH_GRADIENT, dp=1.2, minDist=h4 / 4, param1=100, param2=30,
                                  minRadius=int(0.06 * h4), maxRadius=int(0.35 * h4))
        for x, y, r in (cerchi[0] if cerchi is not None else []):
            X, Y, R = x * M, y * M, r * M
            if (cx - X) ** 2 + (cy - Y) ** 2 <= (0.5 * R) ** 2 and 1.6 * hmed < R < 4 * hmed:
                return ("cerchio", round(X), round(Y), round(R * 1.02 + 1))
        c = cornice(med, cx / M, cy / M, hmed / M)
        if c:
            x, y, w, h = (v * M for v in c)
            # la cornice deve contenere il volto con margine ed essere un riquadro sensato
            if w >= 2.2 * hmed and h >= 2.2 * hmed and 0.4 <= w / h <= 2.5:
                return ("riquadro", int(x), int(y), int(w) // 2 * 2, int(h) // 2 * 2)
    lato = int(3.2 * hmed) // 2 * 2       # webcam senza cornice visibile: un riquadro intorno al volto
    X = int(min(max(cx - lato / 2, 0), W - lato)); Y = int(min(max(cy - lato * 0.4, 0), H - lato))
    return ("riquadro", X, Y, lato, lato)

tratti = []                              # (da, a, tipo)
prev, inizio, n = None, 0, 0
facce, piccoli = [], []

def chiudi(fine):
    tratti.append((inizio / fps, fine / fps, classifica(inizio / fps, fine / fps, facce, piccoli)))

while True:
    ok, img = cap.read()
    if not ok:
        break
    s = cv2.cvtColor(cv2.resize(img, (64, 36), interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2GRAY).astype(np.float32)
    if prev is not None and float(np.mean(np.abs(s - prev))) > SOGLIA_TAGLIO and n - inizio >= 6:
        chiudi(n)
        inizio, facce, piccoli = n, [], []
    prev = s
    if (n - inizio) % PASSO == 0:
        _, v = volti.detect(cv2.resize(img, (W // S, H // S), interpolation=cv2.INTER_AREA))
        if v is not None and len(v):
            x, y, w, h = max(v[:, :4], key=lambda r: r[2] * r[3])
            facce.append((x * S, y * S, w * S, h * S))
        else:
            facce.append(None)
        if len(piccoli) < 16:
            piccoli.append(cv2.resize(img, (W // M, H // M), interpolation=cv2.INTER_AREA))
    n += 1
if n > inizio:
    chiudi(n)

# Una webcam vera torna a lungo nella stessa posizione; una foto inserita per due
# secondi no. Le geometrie (entro 12 px) che in tutto il video coprono meno di 5 s
# (o del 5% della durata) diventano tratti vuoti: meglio l'originale che un falso.
def vicine(p, q):
    return p[0] == q[0] and len(p) == len(q) and all(abs(x - y) <= 12 for x, y in zip(p[1:], q[1:]))
durata_tot = n / fps
coperta = []
for da, a, t in tratti:
    if t[0] in ("cerchio", "riquadro"):
        for g in coperta:
            if vicine(g[0], t): g[1] += a - da; break
        else:
            coperta.append([t, a - da])
soglia = max(5.0, 0.05 * durata_tot)
def ricorrente(t):
    return any(vicine(g[0], t) and g[1] >= soglia for g in coperta)
tratti = [(da, a, t if t[0] not in ("cerchio", "riquadro") or ricorrente(t) else ("vuoto",)) for da, a, t in tratti]

# tratti vicini con la stessa geometria (entro 12 px) si uniscono
uniti = []
for da, a, t in tratti:
    if uniti and uniti[-1][2][0] == t[0] and len(t) == len(uniti[-1][2]) and \
       all(abs(p - q) <= 12 for p, q in zip(t[1:], uniti[-1][2][1:])):
        uniti[-1] = (uniti[-1][0], a, uniti[-1][2])
    else:
        uniti.append((da, a, t))
with open(uscita + ".tmp", "w") as f:
    f.write("# da a tipo parametri (scene.py)\n")
    for da, a, t in uniti:
        f.write(f"{da:.3f} {a:.3f} " + " ".join(str(x) for x in t) + "\n")
os.replace(uscita + ".tmp", uscita)
tot = {}
for da, a, t in uniti: tot[t[0]] = tot.get(t[0], 0) + a - da
print(f"{n} fotogrammi, {len(tratti)} inquadrature, {len(uniti)} tratti: " +
      ", ".join(f"{k} {v:.0f} s" for k, v in sorted(tot.items())))
