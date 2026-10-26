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
# La bibliothèque QRCode est en C antérieur à C23 (elle définit "bool").
gcc -std=gnu11 -O1 -w -c "$libs/QRCode/src/qrcode.c" -o "$build/qrcode.o"
g++ -std=gnu++17 -O1 -w -DARDUINO=100 -I"$here/shim" -I"$libs/Adafruit GFX Library" \
    -I"$libs/ArduinoJson/src" -I"$libs/QRCode/src" -I"$root/include" -I"$root/src" \
    "$here/preview.cpp" "$root/src/station_config.cpp" "$libs/Adafruit GFX Library/Adafruit_GFX.cpp" \
    "$build/qrcode.o" -o "$build/preview"
for case in full:ecran stale:ecran_perime empty:ecran_vide splash:demarrage splashfail:demarrage_echec config:configuration; do
  mode=${case%%:*}
  name=${case#*:}
  [ "$mode" = full ] && arg="" || arg=$mode
  "$build/preview" "$build/$name.pgm" $arg
  python3 -I "$here/topng.py" "$build/$name.pgm" "$root/docs/$name.png"
  echo "docs/$name.png"
done
