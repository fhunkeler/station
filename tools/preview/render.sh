#!/bin/sh
# Compile la prévisualisation et régénère les images de docs/.
# Prérequis : g++, python3 avec Pillow, et les bibliothèques installées par
# PlatformIO (pio run une première fois).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
libs="$root/.pio/libdeps/lolin_d32"
build="$root/.pio/preview"
mkdir -p "$build"
g++ -std=gnu++17 -O1 -w -DARDUINO=100 -I"$here/shim" -I"$libs/Adafruit GFX Library" \
    -I"$libs/ArduinoJson/src" -I"$root/include" \
    "$here/preview.cpp" "$libs/Adafruit GFX Library/Adafruit_GFX.cpp" -o "$build/preview"
for case in full:ecran stale:ecran_perime empty:ecran_vide splash:demarrage splashfail:demarrage_echec; do
  mode=${case%%:*}
  name=${case#*:}
  [ "$mode" = full ] && arg="" || arg=$mode
  "$build/preview" "$build/$name.pgm" $arg
  python3 -I "$here/topng.py" "$build/$name.pgm" "$root/docs/$name.png"
  echo "docs/$name.png"
done
