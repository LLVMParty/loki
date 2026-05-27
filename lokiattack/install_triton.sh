#!/usr/bin/env bash

set -eu

PARALLEL_JOBS=${PARALLEL_JOBS:-1}

if [ "${LOKI_FORCE_TRITON_BUILD:-0}" != "1" ] && python3 -c 'import triton' >/dev/null 2>&1; then
    echo "Triton is already installed. Skipping build."
    exit 0
fi

if ! dpkg -s libboost1.62-all-dev >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y libboost1.62-all-dev
fi

# install capstone (dependency of Triton)
if ! ldconfig -p 2>/dev/null | grep -q 'libcapstone\.so'; then
    if [ ! -d capstone ]; then
        git clone https://github.com/capstone-engine/capstone.git
    fi
    pushd capstone > /dev/null
    git checkout 4.0.2 # 3.0.5 # 4.0.2
    ./make.sh
    sudo ./make.sh install
    sudo ldconfig
    popd > /dev/null
else
    echo "Capstone is already installed. Skipping build."
fi

# install Triton
if [ ! -d Triton ]; then
    git clone https://github.com/JonathanSalwan/Triton
fi
cd Triton
git checkout v0.8.1
rm -rf build
mkdir build ; cd build
# Z3_INCLUDE_DIRS=/home/user/.pyenv/versions/3.9.0/include Z3_LIBRARIES=$(readlink -f ../../loki/z3/build) cmake ..
cmake -DZ3_INTERFACE="" ..
make -j "$PARALLEL_JOBS"
sudo make install
mkdir -p /home/user/.pyenv/versions/3.9.0/lib/python3.9/site-packages
if [ -f /usr/lib/python3/dist-packages/triton.so ]; then
    ln -sf /usr/lib/python3/dist-packages/triton.so /home/user/.pyenv/versions/3.9.0/lib/python3.9/site-packages/triton.so
elif [ -f /usr/local/lib/python3/dist-packages/triton.so ]; then
    ln -sf /usr/local/lib/python3/dist-packages/triton.so /home/user/.pyenv/versions/3.9.0/lib/python3.9/site-packages/triton.so
fi
python3 -c 'import triton'
