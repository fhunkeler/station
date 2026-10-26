"""Convertit un PGM 400x300 de la prévisualisation en PNG façon papier e-paper."""
import sys
from PIL import Image, ImageDraw

src, dst = sys.argv[1], sys.argv[2]
raw = Image.open(src).convert("L").resize((800, 600), Image.NEAREST)
paper, ink = (228, 226, 218), (38, 38, 42)
screen = Image.new("RGB", raw.size, paper)
screen.paste(Image.new("RGB", raw.size, ink), mask=raw.point(lambda v: 255 if v < 128 else 0))
frame = Image.new("RGB", (888, 688), (250, 250, 248))
draw = ImageDraw.Draw(frame)
draw.rounded_rectangle([0, 0, 887, 687], radius=22, fill=(245, 245, 242), outline=(200, 200, 196), width=2)
draw.rectangle([40, 40, 847, 647], fill=(170, 170, 165))
frame.paste(screen, (44, 44))
frame.save(dst)
