#!/bin/sh
# Regenerate the Alef Hebrew fonts in main/fonts from fonts_src/ (Alef, SIL OFL 1.1).
# Requires Node.js (npx downloads lv_font_conv on first use). Run from the repo root.
# Glyphs Alef lacks (Cyrillic, Arabic, LVGL symbols) come from the Noto fonts via LVGL fallback.
set -e
R="0x20-0x7E,0xA0-0xFF,0x2013-0x2014,0x2018-0x201E,0x2022,0x2026,0x590-0x5FF,0xFB1D-0xFB4F"
for sz in 22 32 48; do
  npx --yes lv_font_conv@1.5.2 --font fonts_src/Alef-Regular.ttf -r "$R" --size $sz \
      --format lvgl --bpp 4 --no-compress --lv-include lvgl.h -o main/fonts/font_heb_$sz.c
done
npx --yes lv_font_conv@1.5.2 --font fonts_src/Alef-Bold.ttf -r 0x20,0x2D,0x30-0x3A --size 120 \
    --format lvgl --bpp 4 --no-compress --lv-include lvgl.h -o main/fonts/font_clock_120.c
