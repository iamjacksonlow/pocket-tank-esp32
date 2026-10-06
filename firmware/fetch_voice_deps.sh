#!/bin/sh
# Vendored speech components for the muma voice build (kept out of git: ~155 MB).
# The ESP component registry is not used; both come straight from GitHub.
set -e
cd "$(dirname "$0")/components"
[ -d esp-sr ] || { git clone -q --depth 1 -b v2.0.0 https://github.com/espressif/esp-sr esp-sr; rm -rf esp-sr/.git esp-sr/test_apps esp-sr/docs; mv esp-sr/idf_component.yml esp-sr/idf_component.yml.off; }
[ -d espressif__esp-dsp ] || { git clone -q --depth 1 -b v1.4.14 https://github.com/espressif/esp-dsp espressif__esp-dsp; rm -rf espressif__esp-dsp/.git espressif__esp-dsp/test* espressif__esp-dsp/applications espressif__esp-dsp/docs espressif__esp-dsp/examples; mv espressif__esp-dsp/idf_component.yml espressif__esp-dsp/idf_component.yml.off; }
echo "voice deps ready"
