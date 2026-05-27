#!/usr/bin/env bash

set -eu

PARALLEL_JOBS=${PARALLEL_JOBS:-1}

PIN_DIR=pin-3.23-98579-gb15ab7903-gcc-linux
PIN_FILE="$PIN_DIR.tar.gz"
PIN_URL="https://software.intel.com/sites/landingpage/pintool/downloads/$PIN_FILE"

if [ ! -e "./pin" ]; then
    if [ -d "/opt/$PIN_DIR" ]; then
        sudo chmod -R a+rX "/opt/$PIN_DIR"
        ln -s "/opt/$PIN_DIR" pin
    elif [ -e "/opt/pin" ]; then
        sudo chmod -R a+rX /opt/pin
        ln -s /opt/pin pin
    else
        if [ ! -f "$PIN_FILE" ]; then
            wget "$PIN_URL"
        fi

        if [ ! -d "$PIN_DIR" ]; then
            tar -xzf "$PIN_FILE"
        fi

        ln -s "$PIN_DIR" pin
    fi
fi

# fix PIN_ROOT in makefile
sed -i "s?PIN_ROOT=/opt/pin?PIN_ROOT=$(pwd)/pin?g" makefile

make -j "$PARALLEL_JOBS"
