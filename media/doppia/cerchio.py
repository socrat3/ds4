# Trova dove sta chi parla in un video: il cerchio della webcam sopra uno schermo
# registrato, o nessun cerchio perche' la persona e' a tutto schermo.
#
# 12 fotogrammi sparsi nel video. Se in almeno meta' c'e' un volto grande (alto almeno
# un quarto del fotogramma) la persona e' a tutto schermo: "intero" (i cerchi di Hough
# intorno a una testa grande sarebbero falsi). Altrimenti si cercano cerchi sulla
# mediana dei fotogrammi, dove la webcam resta ferma e lo schermo si media via, e vince
# quello che contiene un volto nella maggior parte dei fotogrammi. Il raggio si allarga
# del 2% + 1 px per coprire l'anello del cerchio originale.
#
# uso: cerchio.py video.mp4 uscita.txt     scrive "cerchio cx cy r" o "intero"; esce 2 se non trova nulla
import os, sys
import cv2, numpy as np

video, uscita = sys.argv[1:3]
# YuNet (OpenCV >= 4.8; il vecchio Haar non c'e' piu' in OpenCV 5): il modello ONNX sta in
# ~/.ds4 o accanto allo script.
modello = "face_detection_yunet_2023mar.onnx"
for d in (os.path.expanduser("~/.ds4"), os.path.dirname(os.path.abspath(__file__))):
    if os.path.exists(os.path.join(d, modello)):
        modello = os.path.join(d, modello)
        break
if not os.path.exists(modello):
    sys.exit("manca face_detection_yunet_2023mar.onnx (in ~/.ds4): opencv_zoo, licenza MIT")
cap = cv2.VideoCapture(video)
tot = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
foto = []
for i in range(12):
    cap.set(cv2.CAP_PROP_POS_FRAMES, int(tot * (i + 0.5) / 12))
    ok, f = cap.read()
    if ok: foto.append(f)
if not foto:
    sys.exit("non leggo fotogrammi dal video")
H, W = foto[0].shape[:2]
volti = cv2.FaceDetectorYN.create(modello, "", (W, H), 0.8, 0.3, 5000)
centri, altezze = [], []
for f in foto:
    _, v = volti.detect(f)
    if v is not None and len(v):
        x, y, w, h = max(v[:, :4], key=lambda r: r[2] * r[3])
        centri.append((x + w / 2, y + h / 2))
        altezze.append(h)
    else:
        centri.append(None)
con_volto = [c for c in centri if c is not None]
if len(con_volto) >= len(foto) / 2 and np.median(altezze) >= 0.25 * H:
    open(uscita, "w").write("intero\n")
    print("intero: volto grande a tutto schermo")
    sys.exit(0)
mediana = np.median(np.stack(foto), axis=0).astype("uint8")
g = cv2.medianBlur(cv2.cvtColor(mediana, cv2.COLOR_BGR2GRAY), 5)
cerchi = cv2.HoughCircles(g, cv2.HOUGH_GRADIENT, dp=1.2, minDist=H / 4, param1=100, param2=40,
                          minRadius=int(0.06 * H), maxRadius=int(0.35 * H))
migliore, voti = None, 0
for cx, cy, r in (cerchi[0] if cerchi is not None else []):
    n = sum(1 for c in con_volto if (c[0] - cx) ** 2 + (c[1] - cy) ** 2 <= r * r)
    if n > voti:
        migliore, voti = (cx, cy, r), n
if migliore is None or voti < len(foto) / 2:
    sys.exit(2)
cx, cy, r = migliore
r = round(r * 1.02 + 1)
open(uscita, "w").write(f"cerchio {round(cx)} {round(cy)} {r}\n")
print(f"cerchio {round(cx)} {round(cy)} {r} ({voti}/{len(foto)} fotogrammi con il volto dentro)")
