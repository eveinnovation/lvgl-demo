# To build:
#    docker build -t lvgl .
#
# To see how to do GUI forwarding in macOS:
#     https://cntnr.io/running-guis-with-docker-on-mac-os-x-a14df6a76efc
#
# To do GUI forwarding on linux, the following may work (easiest method, but unsafe)
#     xhost + && docker run --network=host --env DISPLAY=$DISPLAY lvgl

FROM ubuntu:16.04


WORKDIR /lv

RUN     apt-get -yqq update && \
        apt-get install -yq --no-install-recommends ca-certificates expat libgomp1 && \
        apt-get autoremove -y && \
        apt-get clean -y

ARG DEBIAN_FRONTEND=noninteractive

RUN      buildDeps="autoconf \
                    automake \
                    cmake \
                    curl \
                    bzip2 \
                    libexpat1-dev \
                    g++ \
                    gcc \
                    git \
                    gperf \
                    build-essential \
                    mesa-utils \
                    libgl1-mesa-glx \
                    x11-apps \
                    libtool \
                    make \
                    meson \
                    nasm \
                    perl \
                    pkg-config \
                    python \
                    libssl-dev \
                    libsdl2-dev \
                    yasm \
                    libnuma1 \
                    libnuma-dev \
                    zlib1g-dev" && \
        apt-get -yqq update && \
        apt-get install -yq --no-install-recommends ${buildDeps}

ENV DISPLAY=:0

COPY . /lv

RUN DIR=build && \
    mkdir -p ${DIR}; \
    cd ${DIR} && cmake .. && cmake --build .

CMD ["./build/main"]