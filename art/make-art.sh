#!/bin/sh
# Rebuild the app pictures from the Codex-generated sources in this folder (ImageMagick):
#   app/assets/hermes/plate*.jpg  the stage pictures (eyes closed, open, speaking), 468x702
#   app/sce_sys/icon0.png         the bubble icon, 128x128, 8-bit palette (a 32-bit icon fails to install: 0x8010113D)
cd "$(dirname "$0")/.." || exit 1
magick art/hermes-girl-1-ember.webp -resize 468x702! -quality 88 app/assets/hermes/plate.jpg
magick art/hermes-girl-open-1-ember.webp -resize 468x702! -quality 88 app/assets/hermes/plate_open.jpg
magick art/hermes-girl-talk-1-ember.webp -resize 468x702! -quality 88 app/assets/hermes/plate_talk.jpg
magick art/hermes-icon-1-ember.webp -resize 128x128 -fill none -stroke "rgb(255,107,44)" -strokewidth 3 -draw "rectangle 1,1 126,126" -colors 64 PNG8:app/sce_sys/icon0.png
