#!/bin/bash
# Package SDTracker with all required libraries for distribution

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TRACKER_ROOT="$(cd "$ROOT_DIR/.." && pwd)"

# Output directory
DIST_DIR="$TRACKER_ROOT/SDTracker-Linux-x64"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/lib"

echo "Packaging SDTracker for Linux x64..."

# Copy the main binary
cp "$TRACKER_ROOT/SDTracker" "$DIST_DIR/"
chmod +x "$DIST_DIR/SDTracker"

# Function to copy a library and its dependencies
copy_lib() {
    local lib_name="$1"
    local lib_path=$(ldd "$TRACKER_ROOT/SDTracker" 2>/dev/null | grep "$lib_name" | awk '{print $3}')
    if [ -n "$lib_path" ] && [ -f "$lib_path" ]; then
        cp -L "$lib_path" "$DIST_DIR/lib/"
        echo "  Bundled: $lib_name"
    fi
}

# Copy FFmpeg libraries
echo "Bundling FFmpeg libraries..."
copy_lib "libavformat"
copy_lib "libavcodec"
copy_lib "libavutil"
copy_lib "libswresample"

# Copy FFmpeg's dependencies that might not be on all systems
echo "Bundling FFmpeg dependencies..."
copy_lib "libswscale"
copy_lib "libaom"
copy_lib "libdav1d"
copy_lib "librav1e"
copy_lib "libSvtAv1Enc"
copy_lib "libvpx"
copy_lib "libx264"
copy_lib "libx265"
copy_lib "libmp3lame"
copy_lib "libopus"
copy_lib "libvorbis"
copy_lib "libvorbisenc"
copy_lib "libogg"
copy_lib "libFLAC"
copy_lib "libsoxr"

# Copy LV2 runtime library
echo "Bundling LV2 libraries..."
copy_lib "liblilv"
copy_lib "libserd"
copy_lib "libsord"
copy_lib "libsratom"

# Create launcher script that sets LD_LIBRARY_PATH
cat > "$DIST_DIR/sdtracker.sh" << 'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$SCRIPT_DIR/lib:$LD_LIBRARY_PATH"
exec "$SCRIPT_DIR/SDTracker" "$@"
EOF
chmod +x "$DIST_DIR/sdtracker.sh"

# Create a sample project directory structure
mkdir -p "$DIST_DIR/lgpt/samples"
mkdir -p "$DIST_DIR/lgpt/projects"

# Copy default config files if they exist
if [ -f "$TRACKER_ROOT/lgpt/config.xml" ]; then
    cp "$TRACKER_ROOT/lgpt/config.xml" "$DIST_DIR/lgpt/"
fi
if [ -f "$TRACKER_ROOT/lgpt/mapping.xml" ]; then
    cp "$TRACKER_ROOT/lgpt/mapping.xml" "$DIST_DIR/lgpt/"
fi

echo ""
echo "Package created: $DIST_DIR"
echo ""
echo "Contents:"
ls -la "$DIST_DIR/"
echo ""
echo "Libraries:"
ls -la "$DIST_DIR/lib/" 2>/dev/null || echo "  (none bundled)"
echo ""
echo "To run: ./sdtracker.sh (uses bundled libraries)"
echo "    or: ./SDTracker (uses system libraries)"
