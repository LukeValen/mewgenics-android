#!/usr/bin/env python3
"""Original launcher icon (simple cat face) for all mipmap densities. usage: make_icon.py <res dir>"""
import sys
from PIL import Image, ImageDraw
res = sys.argv[1]
sizes = {'mdpi': 48, 'hdpi': 72, 'xhdpi': 96, 'xxhdpi': 144, 'xxxhdpi': 192}
S = 512
im = Image.new('RGBA', (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
d.rounded_rectangle([16, 16, S - 16, S - 16], radius=110, fill=(58, 32, 84, 255))
d.ellipse([110, 170, 402, 430], fill=(255, 206, 110, 255))
d.polygon([(122, 250), (150, 90), (250, 190)], fill=(255, 206, 110, 255))
d.polygon([(390, 250), (362, 90), (262, 190)], fill=(255, 206, 110, 255))
d.polygon([(150, 210), (165, 130), (220, 195)], fill=(240, 140, 150, 255))
d.polygon([(362, 210), (347, 130), (292, 195)], fill=(240, 140, 150, 255))
d.ellipse([180, 270, 226, 322], fill=(40, 24, 56, 255)); d.ellipse([286, 270, 332, 322], fill=(40, 24, 56, 255))
d.ellipse([196, 280, 210, 294], fill=(255, 255, 255, 255)); d.ellipse([302, 280, 316, 294], fill=(255, 255, 255, 255))
d.polygon([(242, 338), (270, 338), (256, 354)], fill=(220, 110, 120, 255))
d.arc([214, 340, 258, 380], 0, 180, fill=(40, 24, 56, 255), width=6)
d.arc([254, 340, 298, 380], 0, 180, fill=(40, 24, 56, 255), width=6)
for k, s in sizes.items():
    im.resize((s, s), Image.LANCZOS).save('%s/mipmap-%s/ic_launcher.png' % (res, k))
