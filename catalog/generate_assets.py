from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent
IMAGE_DIRECTORY = ROOT / "images"
FONT_DIRECTORY = Path("C:/Windows/Fonts")

INK = "#172226"
PAPER = "#F3F6F4"
WHITE = "#FFFFFF"
TEAL = "#128C88"
CORAL = "#E46453"
YELLOW = "#F1C84B"
MUTED = "#627176"


def load_font(size: int, *, bold: bool = False) -> ImageFont.FreeTypeFont:
    candidates = (
        ["YuGothB.ttc", "seguisb.ttf", "arialbd.ttf"]
        if bold
        else ["YuGothM.ttc", "segoeui.ttf", "arial.ttf"]
    )
    for name in candidates:
        path = FONT_DIRECTORY / name
        if path.exists():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default(size=size)


def draw_waveform(
    draw: ImageDraw.ImageDraw,
    box: tuple[int, int, int, int],
    *,
    color: str,
    width: int,
) -> None:
    left, top, right, bottom = box
    center = (top + bottom) / 2
    height = bottom - top
    points: list[tuple[float, float]] = []
    for x in range(left, right + 1, 3):
        progress = (x - left) / max(1, right - left)
        envelope = math.sin(math.pi * progress) ** 0.72
        wave = math.sin(progress * math.pi * 18) + 0.42 * math.sin(progress * math.pi * 37)
        y = center + wave * height * 0.27 * envelope
        points.append((x, y))
    draw.line(points, fill=color, width=width, joint="curve")


def draw_mouth(
    draw: ImageDraw.ImageDraw,
    center: tuple[int, int],
    size: tuple[int, int],
    openness: float,
    *,
    color: str,
    width: int,
) -> None:
    cx, cy = center
    mouth_width, maximum_height = size
    if openness <= 0.05:
        draw.arc(
            (cx - mouth_width // 2, cy - 5, cx + mouth_width // 2, cy + 9),
            12,
            168,
            fill=color,
            width=width,
        )
        return
    mouth_height = max(width * 2 + 2, int(maximum_height * openness))
    draw.ellipse(
        (
            cx - mouth_width // 2,
            cy - mouth_height // 2,
            cx + mouth_width // 2,
            cy + mouth_height // 2,
        ),
        outline=color,
        width=width,
    )


def create_thumbnail() -> None:
    image = Image.new("RGB", (206, 206), INK)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((18, 18, 188, 125), radius=8, fill="#203137", outline="#31464B", width=2)
    draw_waveform(draw, (33, 39, 173, 101), color=TEAL, width=4)
    draw_mouth(draw, (103, 83), (66, 26), 0.72, color=CORAL, width=4)
    draw.text((18, 140), "NATURAL", font=load_font(24, bold=True), fill=WHITE)
    draw.text((18, 171), "LIP SYNC", font=load_font(16, bold=True), fill=YELLOW)
    image.save(IMAGE_DIRECTORY / "thumbnail.png", optimize=True)


def create_detail() -> None:
    image = Image.new("RGB", (1280, 720), PAPER)
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, 22, 720), fill=TEAL)
    draw.rounded_rectangle((78, 64, 356, 108), radius=8, fill=INK)
    draw.text((98, 73), "AviUtl2 + PSDToolKit2", font=load_font(21, bold=True), fill=WHITE)
    draw.text((76, 144), "Natural LipSync", font=load_font(62, bold=True), fill=INK)
    draw.text((80, 232), "声の流れを捉えて、自然な口形状へ。", font=load_font(28), fill=MUTED)

    draw.rounded_rectangle((80, 322, 620, 610), radius=8, fill=WHITE, outline="#D8E0DC", width=2)
    draw.text((112, 352), "VOICE", font=load_font(18, bold=True), fill=TEAL)
    draw_waveform(draw, (112, 410, 588, 536), color=TEAL, width=5)
    for marker_x in (206, 324, 446, 548):
        draw.line((marker_x, 402, marker_x, 548), fill="#D9E7E4", width=2)
        draw.ellipse((marker_x - 6, 467, marker_x + 6, 479), fill=YELLOW)

    draw.line((650, 466, 712, 466), fill=INK, width=4)
    draw.polygon(((712, 466), (694, 454), (694, 478)), fill=INK)

    draw.rounded_rectangle((746, 322, 1198, 610), radius=8, fill=INK)
    draw.text((778, 352), "MOUTH PATTERNS", font=load_font(18, bold=True), fill=YELLOW)
    centers = [806, 898, 990, 1082, 1164]
    openness = [0.0, 0.23, 0.48, 0.72, 1.0]
    for index, (center_x, state) in enumerate(zip(centers, openness)):
        draw_mouth(draw, (center_x, 464), (58, 58), state, color=CORAL, width=5)
        draw.text((center_x - 7, 540), str(index + 1), font=load_font(17, bold=True), fill=WHITE)

    draw.text((80, 660), "Volume-driven  /  One-step mouth transitions", font=load_font(19), fill=MUTED)
    image.save(IMAGE_DIRECTORY / "detail.png", optimize=True)


def main() -> None:
    IMAGE_DIRECTORY.mkdir(parents=True, exist_ok=True)
    create_thumbnail()
    create_detail()
    print(IMAGE_DIRECTORY / "thumbnail.png")
    print(IMAGE_DIRECTORY / "detail.png")


if __name__ == "__main__":
    main()
