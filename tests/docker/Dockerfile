# What `make test-docker` runs in: mfm's and mwc's build dependencies and grim.
FROM docker.io/library/ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config libwayland-dev wayland-protocols \
    libxkbcommon-dev libcairo2-dev libpango1.0-dev \
    libwlroots-0.19-dev libpixman-1-dev libxcb1-dev libxcb-icccm4-dev \
    grim fonts-dejavu-core fontconfig xkb-data procps util-linux \
    && rm -rf /var/lib/apt/lists/*
# As root, permission checks would not fail where the test wants them to.
RUN useradd -m tester
USER tester
WORKDIR /home/tester
