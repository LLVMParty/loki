# syntax=docker/dockerfile:1.4
### llvm_builder
FROM ubuntu:18.04 AS llvm_builder

ARG TZ=Europe/Berlin
ARG DEBIAN_FRONTEND=noninteractive
RUN --mount=type=secret,id=pro-attach-config,required=false \
    set -eu; \
    attached=0; \
    cleanup() { \
        if [ "$attached" = "1" ]; then \
            pro detach --assume-yes || true; \
            attached=0; \
        fi; \
    }; \
    trap cleanup EXIT; \
    apt-get update; \
    apt-get install -y ca-certificates ubuntu-advantage-tools; \
    if [ -f /run/secrets/pro-attach-config ]; then \
        pro attach --attach-config /run/secrets/pro-attach-config; \
        attached=1; \
        apt-get update; \
        apt-get upgrade -y; \
    fi; \
    apt-get install -y \
        build-essential \
        git \
        make \
        cmake \
        curl \
        gcc \
        ninja-build \
        python3; \
    cleanup; \
    apt-get purge -y ubuntu-advantage-tools; \
    apt-get autoremove -y; \
    rm -rf /var/lib/apt/lists/* /var/lib/ubuntu-advantage /etc/apt/auth.conf.d/90ubuntu-advantage

ARG LLVM_DIR=/llvm_src
ARG LLVM_BUILD_DIR=/llvm_build
ARG LLVM_INSTALL_DIR=/llvm
# LLVM 9.0.0svn, required by loki/translator.
ARG LLVM_COMMIT=e6f22596e5de7f4fc6f1de4725d4aa9b6aeef4aa

# create LLVM 
# increase git pull timeout 
RUN git config --global http.postBuffer 1048576000

WORKDIR $LLVM_DIR
RUN git clone https://github.com/llvm/llvm-project.git
WORKDIR llvm-project
RUN git checkout ${LLVM_COMMIT}

# will be overridden by docker_build.sh script
ARG PARALLEL_JOBS=2
ARG LLVM_PARALLEL_LINK_JOBS=1

# build type used to be RelWithDebInfo (but is 40GB instead of 1.5)
WORKDIR $LLVM_BUILD_DIR
RUN cmake \
  -G "Ninja" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$LLVM_INSTALL_DIR \
  -DLLVM_CCACHE_DIR=$LLVM_DIR/llvm-project/llvm/ccache \
  -DLLVM_ENABLE_CXX1Y=On \
  -DLLVM_ENABLE_IDE=On \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD="X86" \
  -DLLVM_PARALLEL_COMPILE_JOBS=$PARALLEL_JOBS \
  -DLLVM_PARALLEL_LINK_JOBS=$LLVM_PARALLEL_LINK_JOBS \
  -DLLVM_USE_LINKER=gold \
  -DLLVM_BUILD_LLVM_DYLIB=On \
  $LLVM_DIR/llvm-project/llvm \
  && ninja -j ${PARALLEL_JOBS} \
  && ninja install
# end llvm_builder

FROM ubuntu:18.04

# import installed LLVM
COPY --from=llvm_builder /llvm /llvm


ARG DEBIAN_FRONTEND=noninteractive
ARG TZ=Europe/Berlin

RUN --mount=type=secret,id=pro-attach-config,required=false \
    set -eu; \
    attached=0; \
    cleanup() { \
        if [ "$attached" = "1" ]; then \
            pro detach --assume-yes || true; \
            attached=0; \
        fi; \
    }; \
    trap cleanup EXIT; \
    apt-get update; \
    apt-get install -y ca-certificates ubuntu-advantage-tools; \
    if [ -f /run/secrets/pro-attach-config ]; then \
        pro attach --attach-config /run/secrets/pro-attach-config; \
        attached=1; \
        apt-get update; \
        apt-get upgrade -y; \
    fi; \
    apt-get install -y \
        build-essential git \
        curl wget \
        make cmake ninja-build automake \
        locales locales-all \
        sudo \
        neovim tree \
        bear ccache \
        gdb strace ltrace \
        htop \
        parallel psmisc \
        zip unzip \
        screen tmux \
        linux-tools-common linux-tools-generic \
        zsh powerline fonts-powerline \
        libssl-dev zlib1g-dev \
        libbz2-dev libreadline-dev libsqlite3-dev \
        libncursesw5-dev xz-utils tk-dev libxml2-dev libxmlsec1-dev libffi-dev liblzma-dev \
        libboost1.62-all-dev; \
    cleanup; \
    apt-get purge -y ubuntu-advantage-tools; \
    apt-get autoremove -y; \
    rm -rf /var/lib/apt/lists/* /var/lib/ubuntu-advantage /etc/apt/auth.conf.d/90ubuntu-advantage

# MISC NOTES
# * psmisc contains killall

RUN locale-gen en_US.UTF-8
ARG USER_UID=1000
ARG USER_GID=1000

RUN echo "user ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/user \
    && echo "%sudo ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers.d/user \
    && chmod 0440 /etc/sudoers.d/user

WORKDIR /tmp
RUN update-locale LANG=en_US.UTF-8
ENV LANG=en_US.UTF-8

RUN groupadd -g ${USER_GID} user

# add user (-l flag to prevent faillog / lastlog from becoming huge)
RUN useradd -l --shell /bin/bash -c "" -m -u ${USER_UID} -g user -G sudo user

WORKDIR "/home/user"
USER user

# install rust
ARG RUST_TOOLCHAIN=1.71.1
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | bash -s -- -q -y --default-toolchain ${RUST_TOOLCHAIN}
ENV PATH="/home/user/.cargo/bin:${PATH}"

# install PYENV
RUN curl https://pyenv.run | bash
RUN /home/user/.pyenv/bin/pyenv install 3.6.8
RUN /home/user/.pyenv/bin/pyenv install 3.9.0
RUN /home/user/.pyenv/bin/pyenv global 3.9.0
ENV PYENV_ROOT="/home/user/.pyenv"
ENV PATH="/home/user/.pyenv/shims:/home/user/.pyenv/bin:${PATH}"

# zsh agnoster
RUN sh -c "$(wget -O- https://raw.githubusercontent.com/deluan/zsh-in-docker/master/zsh-in-docker.sh)" --     -t agnoster

# Preinstall development dependencies that setup.sh otherwise builds in the mounted workspace.
ARG PARALLEL_JOBS=2
ARG Z3_VERSION=z3-4.8.7
RUN git clone https://github.com/Z3Prover/z3.git /tmp/z3 \
    && cd /tmp/z3 \
    && git checkout ${Z3_VERSION} \
    && python3 scripts/mk_make.py --prefix=/usr \
    && make -C build -j ${PARALLEL_JOBS} \
    && sudo make -C build install \
    && sudo ldconfig \
    && rm -rf /tmp/z3

RUN python3 -m pip install --no-cache-dir \
    wheel \
    orderedset \
    z3-solver==4.8.7.0 \
    future \
    pyparsing==2.2.1

ARG CAPSTONE_VERSION=4.0.2
ARG TRITON_VERSION=v0.8.1
RUN git clone https://github.com/capstone-engine/capstone.git /tmp/capstone \
    && cd /tmp/capstone \
    && git checkout ${CAPSTONE_VERSION} \
    && ./make.sh \
    && sudo ./make.sh install \
    && sudo ldconfig \
    && cd /tmp \
    && rm -rf /tmp/capstone \
    && git clone https://github.com/JonathanSalwan/Triton /tmp/Triton \
    && cd /tmp/Triton \
    && git checkout ${TRITON_VERSION} \
    && mkdir build \
    && cd build \
    && cmake -DZ3_INTERFACE="" .. \
    && make -j ${PARALLEL_JOBS} \
    && sudo make install \
    && mkdir -p /home/user/.pyenv/versions/3.9.0/lib/python3.9/site-packages \
    && if [ -f /usr/lib/python3/dist-packages/triton.so ]; then \
        ln -sf /usr/lib/python3/dist-packages/triton.so /home/user/.pyenv/versions/3.9.0/lib/python3.9/site-packages/triton.so; \
       elif [ -f /usr/local/lib/python3/dist-packages/triton.so ]; then \
        ln -sf /usr/local/lib/python3/dist-packages/triton.so /home/user/.pyenv/versions/3.9.0/lib/python3.9/site-packages/triton.so; \
       fi \
    && python3 -c 'import triton' \
    && rm -rf /tmp/Triton

USER root
ARG PIN_DIR=pin-3.23-98579-gb15ab7903-gcc-linux
RUN wget -O /tmp/${PIN_DIR}.tar.gz https://software.intel.com/sites/landingpage/pintool/downloads/${PIN_DIR}.tar.gz \
    && tar -xzf /tmp/${PIN_DIR}.tar.gz -C /opt \
    && chmod -R a+rX /opt/${PIN_DIR} \
    && ln -s /opt/${PIN_DIR} /opt/pin \
    && rm -f /tmp/${PIN_DIR}.tar.gz
USER user
