#!/usr/bin/env python3
"""效果截图拼图：从整屏截图里裁出几块，存成单块图，再按序号配上说明拼成一张总图。

    python3 scripts/shot_sheet.py artifacts/radar/sheet-infantry.json

规格文件和截图放在同一个目录，路径都相对这个目录：

    {
      "sheet": "changes-infantry.png",
      "title": "第二块 雷达情报 · 步兵 1440×900",
      "parts": [
        {"src": "infantry-1440x900.png", "box": [1083, 150, 356, 330],
         "name": "part-01-敌方花名册-有情报", "label": "敌方标签用实际数，不带「约」"}
      ]
    }

box 是 [x, y, 宽, 高]（像素）。并排对比的两块写在同一个 "row" 里：{"row": [块, 块]}。
用系统 python3（要 Pillow），不用模拟器的 .venv。
"""

import json
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BACKGROUND = (12, 18, 28)
TEXT = (226, 236, 246)
ACCENT = (142, 210, 242)
MARGIN = 24
GAP = 14


def font(size: int) -> ImageFont.FreeTypeFont:
    path = subprocess.run(["fc-match", "-f", "%{file}", "Noto Sans CJK SC:bold"],
                          capture_output=True, text=True, check=True).stdout
    return ImageFont.truetype(path, size)


def crop(folder: Path, part: dict) -> Image.Image:
    x, y, w, h = part["box"]
    image = Image.open(folder / part["src"]).convert("RGB").crop((x, y, x + w, y + h))
    image.save(folder / f"{part['name']}.png")
    return image


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    spec_path = Path(sys.argv[1])
    folder = spec_path.parent
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    title_font, label_font = font(30), font(22)

    blocks: list[tuple[str, list[Image.Image]]] = []
    number = 0
    for entry in spec["parts"]:
        group = entry["row"] if "row" in entry else [entry]
        images = [crop(folder, part) for part in group]
        labels = []
        for part in group:
            number += 1
            labels.append(f"{chr(0x2460 + number - 1)} {part['label']}")
        blocks.append(("    ".join(labels), images))

    width = max(sum(i.width for i in images) + GAP * (len(images) - 1) for _, images in blocks) + 2 * MARGIN
    height = MARGIN + 44
    for _, images in blocks:
        height += 34 + max(i.height for i in images) + GAP * 2
    sheet = Image.new("RGB", (width, height), BACKGROUND)
    draw = ImageDraw.Draw(sheet)
    draw.text((MARGIN, MARGIN), spec["title"], font=title_font, fill=ACCENT)
    y = MARGIN + 44
    for label, images in blocks:
        draw.text((MARGIN, y), label, font=label_font, fill=TEXT)
        y += 34
        x = MARGIN
        for image in images:
            sheet.paste(image, (x, y))
            draw.rectangle((x - 1, y - 1, x + image.width, y + image.height), outline=ACCENT)
            x += image.width + GAP
        y += max(i.height for i in images) + GAP * 2
    sheet.save(folder / spec["sheet"])
    print(f"{folder / spec['sheet']}  {number} 块")
    return 0


if __name__ == "__main__":
    sys.exit(main())
