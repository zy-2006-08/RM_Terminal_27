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


def wrap(text: str, face: ImageFont.FreeTypeFont, width: int) -> list[str]:
    lines, line = [], ""
    for char in text:
        if line and face.getlength(line + char) > width:
            lines.append(line)
            line = char.lstrip()
        else:
            line += char
    return lines + [line] if line else lines


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

    # 每块图上面是它自己的说明,按图的宽度折行(至少 360 像素宽)。
    blocks: list[list[tuple[list[str], Image.Image]]] = []
    number = 0
    for entry in spec["parts"]:
        row = []
        for part in entry["row"] if "row" in entry else [entry]:
            number += 1
            image = crop(folder, part)
            text = f"{chr(0x2460 + number - 1)} {part['label']}"
            row.append((wrap(text, label_font, max(image.width, 360)), image))
        blocks.append(row)

    def row_width(row):
        return sum(max(image.width, 360) for _, image in row) + GAP * (len(row) - 1)

    width = max(max(row_width(row) for row in blocks) + 2 * MARGIN, int(title_font.getlength(spec["title"])) + 2 * MARGIN)
    height = MARGIN + 44
    for row in blocks:
        height += 30 * max(len(lines) for lines, _ in row) + 6 + max(image.height for _, image in row) + GAP * 2
    sheet = Image.new("RGB", (width, height), BACKGROUND)
    draw = ImageDraw.Draw(sheet)
    draw.text((MARGIN, MARGIN), spec["title"], font=title_font, fill=ACCENT)
    y = MARGIN + 44
    for row in blocks:
        label_height = 30 * max(len(lines) for lines, _ in row) + 6
        x = MARGIN
        for lines, image in row:
            for i, line in enumerate(lines):
                draw.text((x, y + 30 * i), line, font=label_font, fill=TEXT)
            sheet.paste(image, (x, y + label_height))
            draw.rectangle((x - 1, y + label_height - 1, x + image.width, y + label_height + image.height), outline=ACCENT)
            x += max(image.width, 360) + GAP
        y += label_height + max(image.height for _, image in row) + GAP * 2
    sheet.save(folder / spec["sheet"])
    print(f"{folder / spec['sheet']}  {number} 块")
    return 0


if __name__ == "__main__":
    sys.exit(main())
