#!/bin/sh
# Generate the sample 9-slice border images.
# Corner slice = 10px, total size 40x40 (center 20x20 is transparent).
set -eu

cd "$(dirname "$0")/.."
mkdir -p assets

make_border() {
    out=$1
    base=$2
    highlight=$3
    inner=$4

    magick -size 40x40 xc:none \
        -fill "$base" -draw 'roundrectangle 0,0 39,39 8,8' \
        -fill none -stroke "$highlight" -strokewidth 1 \
        -draw 'roundrectangle 0.5,0.5 38.5,38.5 7.5,7.5' \
        -stroke "$inner" -draw 'rectangle 9.5,9.5 29.5,29.5' \
        -stroke none \
        -alpha set -channel A -fill black -draw 'rectangle 10,10 29,29' +channel \
        "$out"
}

make_border assets/focused.png   '#ff90b4' '#ffd0e0' '#d95a88'
make_border assets/unfocused.png '#3b4048' '#5b626d' '#24282e'

echo "wrote assets/focused.png assets/unfocused.png"
