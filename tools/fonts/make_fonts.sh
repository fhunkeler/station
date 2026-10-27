#!/bin/sh
# Régénère les polices GFX de include/fonts/ à partir de Barlow Condensed
# (Google Fonts, licence OFL, fichiers TTF dans ce dossier).
# Prérequis : gcc, freetype2, et les bibliothèques installées par PlatformIO
# (pio run une première fois) pour les sources de fontconvert.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
gfx="$root/.pio/libdeps/lolin_d32/Adafruit GFX Library"
build="$root/.pio/fontconvert"
out="$root/include/fonts"
mkdir -p "$build"
gcc -O2 $(pkg-config --cflags freetype2) "$gfx/fontconvert/fontconvert.c" $(pkg-config --libs freetype2) -o "$build/fontconvert"

# convert <graisse> <taille en points> <premier caractère> <dernier caractère>
convert() {
  bits=8; [ "$4" -le 126 ] && bits=7
  "$build/fontconvert" "$here/BarlowCondensed-$1.ttf" "$2" "$3" "$4" > "$out/BarlowCondensed_$1$2pt${bits}b.h"
  echo "include/fonts/BarlowCondensed_$1$2pt${bits}b.h"
}

# Textes : Latin-1 complet (accents, signe degré).
convert Medium 11 32 255
convert SemiBold 11 32 255
convert Medium 14 32 255
convert SemiBold 14 32 255
convert Bold 25 32 255
# Horloges : chiffres et ':' seulement.
convert Bold 48 32 58
