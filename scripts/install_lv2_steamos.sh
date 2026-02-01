#!/usr/bin/env bash
set -euo pipefail

# install_lv2_steamos.sh
# Install LV2 host libraries and common plugin packages on SteamOS (or other distros)
# Usage: sudo ./install_lv2_steamos.sh [--dev] [--dry-run]

DRY_RUN=0
DEV=0

for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --dev) DEV=1 ;;
    -h|--help)
      echo "Usage: $0 [--dev] [--dry-run]"
      echo "  --dev     Install development headers/packages as well"
      echo "  --dry-run Show what would be installed, don't run package manager"
      exit 0 ;;
    *) ;;
  esac
done

run() {
  echo "+ $*"
  if [ "$DRY_RUN" -eq 0 ]; then
    eval "$*"
  fi
}

install_on_pacman() {
  PKGS=(lilv serd sord sratom lv2 lv2-plugins)
  [ "$DEV" -eq 1 ] && PKGS+=(lilv-devel serd-devel sord-devel sratom-devel lv2-devel)

  echo "Detected pacman (Arch/SteamOS). Installing: ${PKGS[*]}"
  run sudo pacman -Syu --noconfirm ${PKGS[*]}
}

install_on_apt() {
  # Debian/Ubuntu names
  PKGS=(liblilv-0-0 libserd-0-0 libsord-0-0 libsratom-0-0 lv2 lv2-plugins)
  DEV_PKGS=(liblilv-dev libserd-dev libsord-dev libsratom-dev lv2-dev)
  echo "Detected APT (Debian/Ubuntu). Installing: ${PKGS[*]}${DEV:+ and dev packages}"
  run sudo apt-get update
  if [ "$DEV" -eq 1 ]; then
    run sudo apt-get install -y ${PKGS[*]} ${DEV_PKGS[*]}
  else
    run sudo apt-get install -y ${PKGS[*]}
  fi
}

install_on_dnf() {
  PKGS=(lilv serd sord sratom lv2 lv2-plugins)
  [ "$DEV" -eq 1 ] && PKGS+=(lilv-devel serd-devel sord-devel sratom-devel lv2-devel)
  echo "Detected DNF (Fedora). Installing: ${PKGS[*]}"
  run sudo dnf install -y ${PKGS[*]}
}

install_on_zypper() {
  PKGS=(lilv serd sord sratom lv2 lv2-plugins)
  echo "Detected zypper (openSUSE). Installing: ${PKGS[*]}"
  run sudo zypper install -y ${PKGS[*]}
}

if command -v pacman >/dev/null 2>&1; then
  install_on_pacman
elif command -v apt-get >/dev/null 2>&1; then
  install_on_apt
elif command -v dnf >/dev/null 2>&1; then
  install_on_dnf
elif command -v zypper >/dev/null 2>&1; then
  install_on_zypper
else
  echo "No supported package manager found. Please install the following packages manually:"
  echo "  lilv, serd, sord, sratom, lv2, lv2-plugins"
  exit 2
fi

echo "Done. If you bundled plugins, set LV2_PATH to point to your plugin folder (e.g. export LV2_PATH=\"/path/to/app/lv2:$LV2_PATH\")."
