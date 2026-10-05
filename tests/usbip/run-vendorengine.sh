#!/bin/sh
# Run the vendor-engine parsing and matching tests.
#
# No Windows, no hardware and no admin rights are needed: everything exercised here is the
# parsing and matching that runs on every platform. The registry read and the process handling
# are Windows-only, are thin, and are checked on a real machine.
set -e
cd "$(dirname "$0")"

OUT="${TMPDIR:-/tmp}/test_vendorengine"

# Qt is the only dependency. pkg-config finds a distro Qt; a bare aqt/Qt install ships no .pc
# file, so qmake is asked instead — which is how this has to run in CI.
if pkg-config --exists Qt6Core 2>/dev/null; then
    QT_FLAGS="$(pkg-config --cflags --libs Qt6Core)"
else
    QMAKE="$(command -v qmake6 || command -v qmake)"
    if [ -z "$QMAKE" ]; then
        echo "no Qt6Core pkg-config and no qmake — cannot build the test here" >&2
        exit 3
    fi
    QTINC="$("$QMAKE" -query QT_INSTALL_HEADERS)"
    QTLIB="$("$QMAKE" -query QT_INSTALL_LIBS)"
    QT_FLAGS="-I$QTINC -I$QTINC/QtCore -L$QTLIB -lQt6Core"
fi

g++ -std=c++17 -fPIC -Wall -Wextra \
    test_vendorengine.cpp ../../app/usbip/vendorengine.cpp \
    $QT_FLAGS \
    -o "$OUT"

exec "$OUT"
