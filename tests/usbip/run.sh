#!/bin/sh
# Run the exporter device-list parser tests.
#
# No hardware, no admin rights and neither usbip tool is needed: the fixtures are
# verbatim captures from real machines. That is the point of keeping the parsers pure.
set -e
cd "$(dirname "$0")"

OUT="${TMPDIR:-/tmp}/test_usbipdevicelist"

# Qt is the only dependency. pkg-config finds a distro Qt; a bare aqt/Qt install ships no .pc
# file, so qmake is asked instead — which is how this has to run in CI.
#
# Without this block the link line below expanded to nothing on the CI image, the build died on
# `QtCore: No such file or directory`, and so this suite simply never ran there — which is how a
# parser that read `Shared (forced)` as NOT shared reached a real machine and took a drive with it.
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
    test_usbipdevicelist.cpp ../../app/usbip/usbipdevicelist.cpp \
    $QT_FLAGS \
    -o "$OUT"

# The link line above decides what this test is BUILT against. It does not decide what the loader
# finds when it RUNS. With a bare aqt/Qt install there is no .pc file, so the flags carry
# `-L$QTLIB` and the binary links cleanly against that Qt — and then the loader searches the
# system paths first, picks up a different libQt6Core, and dies with
# `version 'Qt_6.8' not found (required by ...)`. Put the Qt we linked against in front for the run.
if [ -n "$QTLIB" ]; then
    LD_LIBRARY_PATH="$QTLIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi

exec "$OUT"
