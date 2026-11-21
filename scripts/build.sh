#!/bin/sh
set -eu
ARROW_DIR=${ARROW_DIR:?Set ARROW_DIR to the pyarrow 21.0.0 package directory}
OUTPUT=${1:-shopping-sessions}
${CXX:-g++} -std=c++20 -O3 -DNDEBUG -pthread -Wall -Wextra \
    -isystem "$ARROW_DIR/include" src/main.cpp \
    "$ARROW_DIR/libparquet.so.2100" "$ARROW_DIR/libarrow.so.2100" \
    -Wl,-rpath,"${ARROW_RPATH:-$ARROW_DIR}" -o "$OUTPUT"
