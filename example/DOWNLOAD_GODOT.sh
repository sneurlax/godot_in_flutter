#!/bin/bash
# Download and set up Godot binary

set -e

GODOT_VERSION="4.5.3"
BINARY_DIR="assets/bin"

echo "================================"
echo "Downloading Godot $GODOT_VERSION"
echo "================================"
echo ""

mkdir -p "$BINARY_DIR"

# Try multiple download sources in order
echo "Attempting download from GitHub..."
if wget -q --show-progress https://github.com/godotengine/godot-releases/releases/download/${GODOT_VERSION}-stable/Godot_v${GODOT_VERSION}-stable_linux.x86_64.zip -O /tmp/godot.zip 2>/dev/null; then
    echo "✓ Downloaded from GitHub"
elif wget -q --show-progress https://downloads.godotengine.org/${GODOT_VERSION}/Godot_v${GODOT_VERSION}-stable_linux.x86_64.zip -O /tmp/godot.zip 2>/dev/null; then
    echo "✓ Downloaded from godotengine.org"
else
    echo "ERROR: Could not download Godot from any source"
    echo ""
    echo "Try manually downloading from:"
    echo "  https://godotengine.org/download/linux"
    echo "  Select 4.5.3 and download the x86_64 version"
    echo ""
    echo "Then extract to: $BINARY_DIR/godot"
    exit 1
fi

echo "Extracting..."
unzip -q /tmp/godot.zip -d "$BINARY_DIR/"
cd "$BINARY_DIR"

# Find and rename the extracted binary
if [ -f "Godot_v${GODOT_VERSION}-stable_linux.x86_64" ]; then
    mv "Godot_v${GODOT_VERSION}-stable_linux.x86_64" godot
elif [ -f "Godot" ]; then
    mv "Godot" godot
else
    echo "ERROR: Could not find Godot binary in extracted files"
    ls -la
    exit 1
fi

chmod +x godot
cd - > /dev/null

echo ""
echo "✓ Godot $GODOT_VERSION installed successfully!"
echo "  Location: $(pwd)/$BINARY_DIR/godot"
echo ""
echo "Verify with:"
echo "  ./$BINARY_DIR/godot --version"
echo ""
