from pathlib import Path
OUTPUT = Path(__file__).resolve().parent
from PIL import Image, ImageDraw, ImageFilter
S=1024
im=Image.new('RGBA',(S,S),(0,0,0,0))
d=ImageDraw.Draw(im)
# Dark rounded tile with a restrained blue rim.
d.rounded_rectangle((48,48,976,976),radius=220,fill=(22,25,31,255),outline=(67,79,96,255),width=20)
# Soft chromatic halos: blue is broadest, green intermediate.
def halo(box,color,blur,width):
    layer=Image.new('RGBA',(S,S),(0,0,0,0)); q=ImageDraw.Draw(layer)
    q.ellipse(box,outline=color,width=width)
    return layer.filter(ImageFilter.GaussianBlur(blur))
im=Image.alpha_composite(im,halo((230,230,794,794),(48,126,255,210),28,58))
im=Image.alpha_composite(im,halo((277,277,747,747),(62,193,123,220),18,52))
d=ImageDraw.Draw(im)
# Crisp red focal plane and central aperture.
d.ellipse((326,326,698,698),fill=(27,29,35,255),outline=(239,86,78,255),width=52)
d.ellipse((427,427,597,597),fill=(244,246,249,255))
d.ellipse((474,474,550,550),fill=(229,72,64,255))
# Four clean focus ticks, kept away from the tile edge.
tick=(225,231,239,245); w=30
for a,b in [((512,156),(512,250)),((512,774),(512,868)),((156,512),(250,512)),((774,512),(868,512))]:
    d.line((a,b),fill=tick,width=w)
# Tiny highlight gives the aperture dimensionality without gloss overload.
d.ellipse((456,445,486,475),fill=(255,255,255,210))
# Generate master preview and Windows multi-resolution icon.
im.save(OUTPUT / 'app-icon-1024.png')
im.save(OUTPUT / 'app.ico',format='ICO',sizes=[(16,16),(20,20),(24,24),(32,32),(40,40),(48,48),(64,64),(128,128),(256,256)])
