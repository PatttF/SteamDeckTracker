#!/bin/bash
# Bundle FFmpeg and other required libraries with SDTracker

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST_DIR="$SCRIPT_DIR/../lib"
BINARY="$SCRIPT_DIR/../SDTracker"

# Create lib directory
mkdir -p "$DEST_DIR"

# Get the list of FFmpeg libraries used by the binary
echo "Bundling FFmpeg libraries from $BINARY..."
FFMPEG_LIBS=$(ldd "$BINARY" 2>/dev/null | grep -E "libav|libsw" | awk '{print $3}')

for lib in $FFMPEG_LIBS; do
    if [ -f "$lib" ]; then
        echo "  Copying $lib"
        cp -L "$lib" "$DEST_DIR/"
    fi
done

# Also get dependencies of those libraries (recursive)
for lib in "$DEST_DIR"/*.so*; do
    if [ -f "$lib" ]; then
        DEPS=$(ldd "$lib" 2>/dev/null | grep -E "libav|libsw" | awk '{print $3}')
        for dep in $DEPS; do
            if [ -f "$dep" ] && [ ! -f "$DEST_DIR/$(basename $dep)" ]; then
                echo "  Copying dependency $dep"
                cp -L "$dep" "$DEST_DIR/"
            fi
        done
    fi
done

echo "Done. Libraries copied to $DEST_DIR"
echo ""
echo "Libraries bundled:"
ls -la "$DEST_DIR"
