#!/bin/bash
# Download and set up Godot binary for the flutter_godot example project.
#
# The Godot binary is required to run the Linux example but is not checked
# into version control due to its size (~130 MB).
#
# Usage:
#   ./DOWNLOAD_GODOT.sh              # Downloads default version (4.5-stable)
#   GODOT_VERSION=4.5.1 ./DOWNLOAD_GODOT.sh  # Downloads specific version

set -e

GODOT_VERSION="${GODOT_VERSION:-4.5-stable}"
BINARY_DIR="assets/bin"

# Normalize version string for download URL
# "4.5-stable" -> download tag "4.5-stable", filename "4.5-stable"
# "4.5.1" -> download tag "4.5.1-stable", filename "4.5.1-stable"
if [[ "$GODOT_VERSION" == *-stable ]]; then
    DOWNLOAD_TAG="$GODOT_VERSION"
else
    DOWNLOAD_TAG="${GODOT_VERSION}-stable"
fi

echo "================================"
echo "Downloading Godot $GODOT_VERSION"
echo "================================"
echo ""

mkdir -p "$BINARY_DIR"

FILENAME="Godot_v${DOWNLOAD_TAG}_linux.x86_64"

echo "Attempting download from GitHub..."
if wget -q --show-progress "https://github.com/godotengine/godot/releases/download/${DOWNLOAD_TAG}/${FILENAME}.zip" -O /tmp/godot.zip 2>/dev/null; then
    echo "Downloaded from GitHub"
elif wget -q --show-progress "https://github.com/godotengine/godot-builds/releases/download/${DOWNLOAD_TAG}/${FILENAME}.zip" -O /tmp/godot.zip 2>/dev/null; then
    echo "Downloaded from GitHub (godot-builds)"
elif wget -q --show-progress "https://downloads.tuxfamily.org/godotengine/${GODOT_VERSION}/${FILENAME}.zip" -O /tmp/godot.zip 2>/dev/null; then
    echo "Downloaded from TuxFamily mirror"
else
    echo "ERROR: Could not download Godot $GODOT_VERSION from any source."
    echo ""
    echo "Download manually from: https://godotengine.org/download/linux"
    echo "Then place the x86_64 binary at: $BINARY_DIR/godot"
    exit 1
fi

echo "Extracting..."
unzip -qo /tmp/godot.zip -d "$BINARY_DIR/"
rm -f /tmp/godot.zip

# Find and rename the extracted binary
cd "$BINARY_DIR"
if [ -f "$FILENAME" ]; then
    mv "$FILENAME" godot
else
    # Try to find any Godot binary
    FOUND=$(ls Godot_v* 2>/dev/null | head -1)
    if [ -n "$FOUND" ]; then
        mv "$FOUND" godot
    else
        echo "ERROR: Could not find Godot binary in extracted files."
        ls -la
        exit 1
    fi
fi

chmod +x godot
cd - > /dev/null

echo ""
echo "Godot installed at: $(pwd)/$BINARY_DIR/godot"
echo ""
echo "Verify with:"
echo "  ./$BINARY_DIR/godot --version"
echo ""
