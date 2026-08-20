from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
ASSETS.mkdir(parents=True, exist_ok=True)

canvas_size = 1024
image = Image.new("RGBA", (canvas_size, canvas_size), (0, 0, 0, 0))
draw = ImageDraw.Draw(image)
draw.rounded_rectangle(
    (64, 64, 960, 960), radius=205, fill=(232, 91, 63, 255)
)

font_path = Path("C:/Windows/Fonts/segoeuib.ttf")
font = ImageFont.truetype(str(font_path), 600)
draw.text(
    (512, 500), "M", font=font, fill=(255, 255, 255, 255),
    anchor="mm", stroke_width=0
)

png_path = ASSETS / "MdViewer.png"
ico_path = ASSETS / "MdViewer.ico"
image.save(png_path, optimize=True)
image.save(
    ico_path,
    format="ICO",
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
           (64, 64), (128, 128), (256, 256)],
)

print(f"Generated {png_path}")
print(f"Generated {ico_path}")
