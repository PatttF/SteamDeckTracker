#ifndef _SAMPLE_EDITOR_VIEW_H_
#define _SAMPLE_EDITOR_VIEW_H_

#include "BaseClasses/View.h"
#include "ViewData.h"

#define MAX_SLICE_COUNT 48

class SampleEditorView : public View {
public:
    SampleEditorView(GUIWindow &w, ViewData *viewData);
    ~SampleEditorView();

    virtual void ProcessButtonMask(unsigned short mask, bool pressed);
    virtual void DrawView();
    virtual void DrawGraphics();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int tick = 0);
    virtual void OnFocus();

protected:
    void processNormalButtonMask(unsigned short mask);
    void moveCursorAndAutoScroll(int delta);
    void drawWaveformGraphical();
    void drawSliceMarkersGraphical();
    void drawInfo();
    void drawHelp();

    // Zoom and navigation
    void zoomIn();
    void zoomOut();
    void scrollLeft();
    void scrollRight();
    void scrollToSlice(int slice);

    // Slice editing
    void addSlice();
    void removeSlice();
    void moveSliceCursor(int dir);

    // Operations
    void setEndPoint();
    void setStartPoint();

    // Playback preview
    void playSlice(int sliceIndex);
    void playFromStart();
    void stopPlayback();
    bool isPlaying();

    // Auto-configure instrument for manual slicing
    void autoSetSlicerMode();

    // Utility
    int sampleToScreen(int samplePos);
    int screenToSample(int screenX);
    int getSampleSize();
    short *getSampleBuffer();

    // Color helpers for pixel drawing
    void getColorForDef(ColorDefinition cd, unsigned short &r, unsigned short &g, unsigned short &b);

private:
    // Zoom state
    int viewStart_;      // First visible sample frame
    int viewEnd_;        // Last visible sample frame
    int zoomLevel_;      // 0 = full view, increases = more zoom

    // Cursor
    int cursorX_;        // Cursor column on screen (0-based, within waveform pixel area)

    // Slice editing
    int sliceCount_;                      // Number of manual slices
    int slicePoints_[MAX_SLICE_COUNT];    // Frame positions of manual slice markers
    int currentSlice_;                    // Currently selected slice marker index (-1 = none)

    // Waveform pixel display area (in app-pixel coordinates, 550x240 space)
    // Character grid is 80x30, each char is 8x8 pixels
    // We use rows 2-24 (characters) for the waveform = pixels 16 to 200
    // And columns 1-78 = pixels 8 to 632... but app width is 550
    // So use columns 1-67 = pixels 8 to 544
    static const int WAVE_PX_X = 8;       // Left edge in app-pixels
    static const int WAVE_PX_Y = 16;      // Top edge in app-pixels (row 2)
    static const int WAVE_PX_W = 536;     // Width in app-pixels (67 chars)
    static const int WAVE_PX_H = 176;     // Height in app-pixels (22 rows)

    // Text area coordinates (in character cells)
    static const int INFO_Y = 25;         // Info line row (char coords)
    static const int HELP_Y = 26;         // Help text start row

    // Mode
    enum EditorMode {
        MODE_NAVIGATE,   // Zoom/scroll waveform
        MODE_SLICE       // Place/move/delete slice markers
    };
    EditorMode mode_;

    unsigned short lastMask_;

    // Playback state
    bool playing_;
    int playChannel_;
};

#endif
