#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script as root."
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y \
  autoconf \
  automake \
  autopoint \
  bash \
  bison \
  bzip2 \
  cmake \
  flex \
  g++ \
  g++-multilib \
  gcc-mingw-w64-x86-64 \
  g++-mingw-w64-x86-64 \
  gettext \
  git \
  gperf \
  gtk-doc-tools \
  intltool \
  libc6-dev-i386 \
  imagemagick \
  libclang-dev \
  libgdk-pixbuf-2.0-dev \
  libgl-dev \
  libopengl-dev \
  libltdl-dev \
  libpcre2-dev \
  libssl-dev \
  libtool \
  libtool-bin \
  libxml-parser-perl \
  lzip \
  make \
  mercurial \
  mingw-w64 \
  ninja-build \
  openssl \
  p7zip-full \
  patch \
  perl \
  python-is-python3 \
  python3 \
  python3-mako \
  python3-packaging \
  python3-pkg-resources \
  python3-setuptools \
  ruby \
  sed \
  sqlite3 \
  unzip \
  wget \
  wine \
  wine64 \
  xz-utils \
  zip
