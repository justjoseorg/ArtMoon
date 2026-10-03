#!/bin/sh
# Run the exporter device-list parser tests.
#
# No hardware, no admin rights and neither usbip tool is needed: the fixtures are
# verbatim captures from real machines. That is the point of keeping the parsers pure.
set -e
cd "$(dirname "$0")"

OUT="${TMPDIR:-/tmp}/test_usbipdevicelist"

g++ -std=c++17 -fPIC -Wall -Wextra \
    test_usbipdevicelist.cpp ../../app/usbip/usbipdevicelist.cpp \
    $(pkg-config --cflags --libs Qt6Core) \
    -o "$OUT"

exec "$OUT"
