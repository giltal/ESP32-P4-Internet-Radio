#!/bin/sh
# Weather icon fonts from FontAwesome 5 Free (fonts_src/, copied from LVGL's scripts/built_in_font;
# Font Awesome Free: icons CC BY 4.0, font SIL OFL 1.1). Run from the repo root (Node.js required).
#   F185 sun  F186 moon  F6C4 cloud-sun  F6C3 cloud-moon  F0C2 cloud  F75F smog
#   F73D cloud-rain  F740 cloud-showers-heavy  F743 cloud-sun-rain  F2DC snowflake
#   F0E7 bolt  F043 tint (droplet)  F72E wind
set -e
R="0xF185,0xF186,0xF6C4,0xF6C3,0xF0C2,0xF75F,0xF73D,0xF740,0xF743,0xF2DC,0xF0E7,0xF043,0xF72E"
for sz in 24 32 64; do
  npx --yes lv_font_conv@1.5.2 --font fonts_src/FontAwesome5-Solid+Brands+Regular.woff -r "$R" --size $sz \
      --format lvgl --bpp 4 --no-compress --lv-include lvgl.h -o main/fonts/font_wx_$sz.c
done
