#!/bin/bash
# Install or update FrameSize (static aarch64) for the Steam Frame:
#
#   curl -fsSL https://raw.githubusercontent.com/Skillfur/FrameSize/HEAD/install.sh | bash
#
# Downloads the binary from the latest release of this repository to
# ~/Applications/FrameSize, makes it executable and adds a FrameSize entry
# to the application menu. Running it again updates FrameSize.
set -euo pipefail

REPO="Skillfur/FrameSize"
APPS_DIR="$HOME/Applications"
APP="$APPS_DIR/FrameSize"
DESKTOP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
DESKTOP_FILE="$DESKTOP_DIR/framesize.desktop"
ICON="${XDG_DATA_HOME:-$HOME/.local/share}/icons/framesize.png"

die() {
  echo "error: $*" >&2
  exit 1
}

# Everything runs from main, so a download cut short by `curl | bash` cannot
# execute half a script.
main() {
  [ "$(uname -m)" = aarch64 ] || die "this build of FrameSize is for aarch64 (ARM64) only, this machine is $(uname -m)"
  command -v curl > /dev/null || die "curl is required"

  echo "Looking up the latest release of $REPO..."
  local release urls url sha_url tag
  release=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest") ||
    die "could not read the latest release of $REPO (is there a published release?)"
  tag=$(grep -o '"tag_name": *"[^"]*"' <<< "$release" | head -n 1 | cut -d'"' -f4)
  urls=$(grep -o '"browser_download_url": *"[^"]*"' <<< "$release" | cut -d'"' -f4)
  url=$(grep -E '/FrameSize-aarch64$' <<< "$urls" | head -n 1) ||
    die "release $tag has no FrameSize-aarch64 binary"
  sha_url=$(grep -E '/FrameSize-aarch64\.sha256$' <<< "$urls" | head -n 1) || true

  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT

  echo "Downloading $(basename "$url") ($tag)..."
  curl -fL --progress-bar -o "$tmp/FrameSize-aarch64" "$url"
  if [ -n "$sha_url" ]; then
    local expected actual
    expected=$(curl -fsSL "$sha_url" | cut -d' ' -f1)
    actual=$(sha256sum "$tmp/FrameSize-aarch64" | cut -d' ' -f1)
    [ "$expected" = "$actual" ] || die "checksum mismatch for the downloaded binary"
    echo "Checksum OK."
  fi
  chmod 755 "$tmp/FrameSize-aarch64"

  # Move into place instead of overwriting, so a running FrameSize is not disturbed.
  mkdir -p "$APPS_DIR"
  cp "$tmp/FrameSize-aarch64" "$APP.new"
  mv -f "$APP.new" "$APP"
  echo "Installed $APP"

  local icon_line=""
  if curl -fsSL -o "$tmp/icon.png" "https://raw.githubusercontent.com/$REPO/HEAD/icon.png"; then
    mkdir -p "$(dirname "$ICON")"
    cp "$tmp/icon.png" "$ICON"
    icon_line="Icon=$ICON"
  fi

  mkdir -p "$DESKTOP_DIR"
  cat > "$DESKTOP_FILE" << EOF
[Desktop Entry]
Type=Application
Name=FrameSize
GenericName=Xwayland window resizer
Comment=Set the real buffer size of Xwayland windows on the Steam Frame
Exec="$APP"
$icon_line
Terminal=false
Categories=Game;Utility;
StartupWMClass=FrameSize
EOF
  if command -v update-desktop-database > /dev/null; then
    update-desktop-database "$DESKTOP_DIR" > /dev/null 2>&1 || true
  fi
  echo "Added menu entry $DESKTOP_FILE"
  echo
  echo "Done. Start FrameSize from the application menu, or add $APP to Steam as a non-Steam game."
}

main "$@"
