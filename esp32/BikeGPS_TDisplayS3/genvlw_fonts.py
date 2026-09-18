from PIL import Image, ImageDraw, ImageFont
import struct

TTF = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

def build(px, chars):
    font = ImageFont.truetype(TTF, px)
    ascent, descent = font.getmetrics()
    glyphs, bitmaps = [], bytearray()
    for ch in chars:
        bbox = font.getbbox(ch, anchor="ls")
        x0, y0, x1, y1 = bbox
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        adv = int(round(font.getlength(ch)))
        if w == 0 or h == 0:
            glyphs.append((ord(ch), 0, 0, adv, 0, 0, 0)); continue
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
        glyphs.append((ord(ch), h, w, adv, -y0, x0, 0))
        bitmaps += img.tobytes()
    out = bytearray()
    out += struct.pack(">IIIIII", len(glyphs), 11, px, 0, ascent, descent)
    for g in glyphs:
        out += struct.pack(">Iiiiiii", *g)
    out += bitmaps
    return bytes(out)

fonts = [
    ("VLW_LABEL", 14, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .:-+/"),
    ("VLW_VALUE", 26, "0123456789.:- "),
    ("VLW_BIG",   42, "0123456789.- "),
    ("VLW_SPEED", 56, "0123456789.- "),
]

lines = ["// Generato automaticamente da /tmp/genvlw.py - font VLW antialiased (TFT_eSPI smooth font)",
         "// DejaVu Sans Bold; header 24 B + 28 B/glifo + bitmap alpha 8-bit", ""]
tot = 0
for name, px, chars in fonts:
    data = build(px, chars)
    tot += len(data)
    lines.append(f"// {name}: {px} px, {len(chars)} glifi, {len(data)} byte")
    lines.append(f"const uint8_t {name}[] PROGMEM = {{")
    for i in range(0, len(data), 16):
        lines.append("  " + ",".join(f"0x{b:02X}" for b in data[i:i+16]) + ",")
    lines.append("};")
    lines.append("")

open("/home/flavio/AndroidStudioProjects/GpsPosition/esp32/BikeGPS_TDisplayS3/vlw_fonts.h", "w").write("\n".join(lines))
print("vlw_fonts.h generato,", tot, "byte di dati")
