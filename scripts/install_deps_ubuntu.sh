#!/usr/bin/env bash
set -euo pipefail

PACKAGES=(
  build-essential
  cmake
  ninja-build
  qt6-base-dev
  qt6-base-dev-tools
  qt6-tools-dev
  qt6-tools-dev-tools
)

missing=()
for pkg in "${PACKAGES[@]}"; do
  # Fast pre-check with keyword as requested, then exact dpkg status check.
  if ! dpkg -l | grep -i -q "${pkg}"; then
    echo "[keyword-miss] ${pkg}"
  fi
  if dpkg -l | grep -i -q "^ii[[:space:]]\+${pkg}[[:space:]]"; then
    echo "[ok] ${pkg} already installed"
  else
    echo "[missing] ${pkg}"
    missing+=("${pkg}")
  fi
done

if [[ ${#missing[@]} -eq 0 ]]; then
  echo "All dependencies are installed."
  exit 0
fi

echo "Installing missing packages: ${missing[*]}"
sudo apt-get update
sudo apt-get install -y "${missing[@]}"
