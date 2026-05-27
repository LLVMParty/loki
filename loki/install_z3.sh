#!/usr/bin/env bash

set -eu

PARALLEL_JOBS=${PARALLEL_JOBS:-1}
Z3_VERSION=${Z3_VERSION:-z3-4.8.7}

z3_installed() {
    command -v z3 >/dev/null 2>&1 \
        && [ -f /usr/include/z3.h ] \
        && ldconfig -p 2>/dev/null | grep -q 'libz3\.so' \
        && python3 -c 'import z3' >/dev/null 2>&1
}

if [ "${LOKI_FORCE_Z3_BUILD:-0}" != "1" ] && z3_installed; then
    echo "Z3 is already installed. Skipping build."
    exit 0
fi

if [ ! -d z3 ]; then
    git clone https://github.com/Z3Prover/z3.git
else
    echo "Directory z3 already exists. Not cloning.."
fi

cd z3
git checkout "$Z3_VERSION"

# Install Python bindings into the active Python environment.
python3 scripts/mk_make.py --python
cd build
make -j "$PARALLEL_JOBS"
sudo make install

# Install C headers/library to /usr for z3-sys and system linkers.
cd ..
rm -rf build
python3 scripts/mk_make.py --prefix=/usr
cd build
make -j "$PARALLEL_JOBS"
sudo make install
sudo ldconfig
