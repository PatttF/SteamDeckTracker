#include "SampleEditorView.h"
#include "Application/Instruments/SampleInstrument.h"
#include "Application/Instruments/SamplePool.h"
#include "Application/Instruments/SoundSource.h"
#include "Application/Instruments/InstrumentBank.h"
#include "Application/AppWindow.h"
#include "Application/Player/PlayerMixer.h"
#include "Application/Player/PlayerChannel.h"
#include "Application/Utils/char.h"
#include "Adapters/SDL2/GUI/SDLGUIWindowImp.h"
#include "System/Console/Trace.h"
#include <string.h>
#include <math.h>
#include <algorithm>
#include <stdlib.h>

SampleEditorView::SampleEditorView(GUIWindow &w, ViewData *viewData)
    : View(w, viewData) {

    viewStart_ = 0;
    viewEnd_ = 0;
    zoomLevel_ = 0;
    cursorX_ = WAVE_PX_W / 2;
    sliceCount_ = 0;
    currentSlice_ = -1;
    mode_ = MODE_NAVIGATE;
    lastMask_ = 0;
    playing_ = false;
    playChannel_ = 0;

    for (int i = 0; i < MAX_SLICE_COUNT; i++) {
        slicePoints_[i] = 0;
    }
}

SampleEditorView::~SampleEditorView() {
}

short *SampleEditorView::getSampleBuffer() {
    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (!instr || instr->GetType() != IT_SAMPLE) return nullptr;

    SampleInstrument *si = (SampleInstrument *)instr;
    int sampleIdx = si->GetSampleIndex();
    if (sampleIdx < 0) return nullptr;

    SamplePool *pool = SamplePool::GetInstance();
    SoundSource *src = pool->GetSource(sampleIdx);
    if (!src) return nullptr;

    return (short *)src->GetSampleBuffer(-1);
}

int SampleEditorView::getSampleSize() {
    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (!instr || instr->GetType() != IT_SAMPLE) return 0;

    SampleInstrument *si = (SampleInstrument *)instr;
    int sampleIdx = si->GetSampleIndex();
    if (sampleIdx < 0) return 0;

    SamplePool *pool = SamplePool::GetInstance();
    SoundSource *src = pool->GetSource(sampleIdx);
    if (!src) return 0;

    return src->GetSize(-1);
}

int SampleEditorView::sampleToScreen(int samplePos) {
    if (viewEnd_ <= viewStart_) return 0;
    long long pos = (long long)(samplePos - viewStart_) * WAVE_PX_W;
    return (int)(pos / (viewEnd_ - viewStart_));
}

int SampleEditorView::screenToSample(int screenX) {
    if (WAVE_PX_W <= 0) return viewStart_;
    long long range = viewEnd_ - viewStart_;
    return viewStart_ + (int)((long long)screenX * range / WAVE_PX_W);
}

void SampleEditorView::zoomIn() {
    int totalSize = getSampleSize();
    if (totalSize <= 0) return;

    int center = screenToSample(cursorX_);
    int range = viewEnd_ - viewStart_;
    int newRange = range / 2;
    if (newRange < WAVE_PX_W) newRange = WAVE_PX_W;

    viewStart_ = center - newRange / 2;
    viewEnd_ = viewStart_ + newRange;

    if (viewStart_ < 0) {
        viewStart_ = 0;
        viewEnd_ = newRange;
    }
    if (viewEnd_ > totalSize) {
        viewEnd_ = totalSize;
        viewStart_ = viewEnd_ - newRange;
        if (viewStart_ < 0) viewStart_ = 0;
    }

    zoomLevel_++;
    isDirty_ = true;
}

void SampleEditorView::zoomOut() {
    int totalSize = getSampleSize();
    if (totalSize <= 0) return;

    if (zoomLevel_ <= 0) {
        viewStart_ = 0;
        viewEnd_ = totalSize;
        zoomLevel_ = 0;
        isDirty_ = true;
        return;
    }

    int center = screenToSample(cursorX_);
    int range = viewEnd_ - viewStart_;
    int newRange = range * 2;
    if (newRange > totalSize) newRange = totalSize;

    viewStart_ = center - newRange / 2;
    viewEnd_ = viewStart_ + newRange;

    if (viewStart_ < 0) {
        viewStart_ = 0;
        viewEnd_ = newRange;
    }
    if (viewEnd_ > totalSize) {
        viewEnd_ = totalSize;
        viewStart_ = viewEnd_ - newRange;
        if (viewStart_ < 0) viewStart_ = 0;
    }

    zoomLevel_--;
    if (zoomLevel_ < 0) zoomLevel_ = 0;
    isDirty_ = true;
}

void SampleEditorView::scrollLeft() {
    int range = viewEnd_ - viewStart_;
    int step = range / 4;
    if (step < 1) step = 1;

    viewStart_ -= step;
    viewEnd_ -= step;
    if (viewStart_ < 0) {
        viewStart_ = 0;
        viewEnd_ = range;
    }
    isDirty_ = true;
}

void SampleEditorView::scrollRight() {
    int totalSize = getSampleSize();
    if (totalSize <= 0) return;

    int range = viewEnd_ - viewStart_;
    int step = range / 4;
    if (step < 1) step = 1;

    viewStart_ += step;
    viewEnd_ += step;
    if (viewEnd_ > totalSize) {
        viewEnd_ = totalSize;
        viewStart_ = viewEnd_ - range;
        if (viewStart_ < 0) viewStart_ = 0;
    }
    isDirty_ = true;
}

void SampleEditorView::scrollToSlice(int slice) {
    if (slice < 0 || slice >= sliceCount_) return;
    int totalSize = getSampleSize();
    if (totalSize <= 0) return;

    int range = viewEnd_ - viewStart_;
    int center = slicePoints_[slice];
    viewStart_ = center - range / 2;
    viewEnd_ = viewStart_ + range;

    if (viewStart_ < 0) {
        viewStart_ = 0;
        viewEnd_ = range;
    }
    if (viewEnd_ > totalSize) {
        viewEnd_ = totalSize;
        viewStart_ = viewEnd_ - range;
        if (viewStart_ < 0) viewStart_ = 0;
    }
    isDirty_ = true;
}

void SampleEditorView::autoSetSlicerMode() {
    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (!instr || instr->GetType() != IT_SAMPLE) return;

    SampleInstrument *si = (SampleInstrument *)instr;
    // Set loop mode to slicer (SILM_SLICE = 5)
    Variable *loopMode = si->FindVariable(SIP_LOOPMODE);
    if (loopMode && loopMode->GetInt() != SILM_SLICE) {
        loopMode->SetInt(SILM_SLICE);
    }
    // Set slices to 0 (manual mode)
    Variable *slices = si->FindVariable(SIP_SLICES);
    if (slices && slices->GetInt() != 0) {
        slices->SetInt(0);
    }
}

void SampleEditorView::addSlice() {
    if (sliceCount_ >= MAX_SLICE_COUNT) return;

    int pos = screenToSample(cursorX_);
    int totalSize = getSampleSize();
    if (pos <= 0 || pos >= totalSize) return;

    // Insert in sorted order
    int insertIdx = sliceCount_;
    for (int i = 0; i < sliceCount_; i++) {
        if (slicePoints_[i] == pos) return; // Already exists
        if (slicePoints_[i] > pos) {
            insertIdx = i;
            break;
        }
    }

    // Shift everything after insertIdx
    for (int i = sliceCount_; i > insertIdx; i--) {
        slicePoints_[i] = slicePoints_[i - 1];
    }
    slicePoints_[insertIdx] = pos;
    sliceCount_++;
    currentSlice_ = insertIdx;

    // Store to instrument
    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (instr && instr->GetType() == IT_SAMPLE) {
        SampleInstrument *si = (SampleInstrument *)instr;
        si->SetSliceCount(sliceCount_);
        for (int i = 0; i < sliceCount_; i++) {
            si->SetSlicePoint(i, slicePoints_[i]);
        }
    }

    // Automatically set loop mode to slicer and slices to manual
    autoSetSlicerMode();

    isDirty_ = true;
}

void SampleEditorView::removeSlice() {
    if (sliceCount_ <= 0 || currentSlice_ < 0 || currentSlice_ >= sliceCount_)
        return;

    // Shift everything after currentSlice_
    for (int i = currentSlice_; i < sliceCount_ - 1; i++) {
        slicePoints_[i] = slicePoints_[i + 1];
    }
    sliceCount_--;
    if (currentSlice_ >= sliceCount_) {
        currentSlice_ = sliceCount_ - 1;
    }

    // Store to instrument
    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (instr && instr->GetType() == IT_SAMPLE) {
        SampleInstrument *si = (SampleInstrument *)instr;
        si->SetSliceCount(sliceCount_);
        for (int i = 0; i < sliceCount_; i++) {
            si->SetSlicePoint(i, slicePoints_[i]);
        }
    }

    isDirty_ = true;
}

void SampleEditorView::moveSliceCursor(int dir) {
    if (sliceCount_ <= 0) return;

    currentSlice_ += dir;
    if (currentSlice_ < 0) currentSlice_ = sliceCount_ - 1;
    if (currentSlice_ >= sliceCount_) currentSlice_ = 0;

    // Move screen cursor to this slice position
    int screenPos = sampleToScreen(slicePoints_[currentSlice_]);
    if (screenPos >= 0 && screenPos < WAVE_PX_W) {
        cursorX_ = screenPos;
    } else {
        // Scroll to make slice visible
        scrollToSlice(currentSlice_);
        cursorX_ = sampleToScreen(slicePoints_[currentSlice_]);
    }

    isDirty_ = true;
}

void SampleEditorView::setEndPoint() {
    // Set end point at cursor position
    int pos = screenToSample(cursorX_);
    int totalSize = getSampleSize();
    if (pos <= 0) pos = 1;
    if (pos > totalSize) pos = totalSize;

    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (!instr || instr->GetType() != IT_SAMPLE) return;

    SampleInstrument *si = (SampleInstrument *)instr;
    Variable *startVar = si->FindVariable(SIP_START);
    int startPos = startVar ? startVar->GetInt() : 0;

    // End must be after start
    if (pos <= startPos) {
        SetNotification("End must be after start");
        return;
    }

    Variable *endVar = si->FindVariable(SIP_END);
    if (endVar) endVar->SetInt(pos);

    SetNotification("End set");
    isDirty_ = true;
}

void SampleEditorView::setStartPoint() {
    // Set start point at cursor position
    int pos = screenToSample(cursorX_);
    int totalSize = getSampleSize();
    if (pos < 0) pos = 0;
    if (pos >= totalSize) pos = totalSize - 1;

    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (!instr || instr->GetType() != IT_SAMPLE) return;

    SampleInstrument *si = (SampleInstrument *)instr;
    Variable *endVar = si->FindVariable(SIP_END);
    int endPos = endVar ? endVar->GetInt() : totalSize;

    // Start must be before end
    if (pos >= endPos) {
        SetNotification("Start must be before end");
        return;
    }

    Variable *startVar = si->FindVariable(SIP_START);
    if (startVar) startVar->SetInt(pos);

    SetNotification("Start set");
    isDirty_ = true;
}

void SampleEditorView::playSlice(int sliceIndex) {
    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (!instr || instr->GetType() != IT_SAMPLE) return;

    SampleInstrument *si = (SampleInstrument *)instr;
    int totalSize = getSampleSize();

    // Ensure slicer mode is active (playFromStart may have set oneshot)
    autoSetSlicerMode();

    // Ensure loopEnd covers the whole file so SILM_SLICE uses correct wavSize
    Variable *endVar = si->FindVariable(SIP_END);
    int savedEnd = endVar ? endVar->GetInt() : totalSize;
    if (endVar) endVar->SetInt(totalSize);

    Variable *rootVar = si->FindVariable(SIP_ROOTNOTE);
    int rootNote = rootVar ? rootVar->GetInt() : 60;

    int channel = viewData_->songX_;
    // Slice point N divides regions N and N+1
    // Region after slice point N = region N+1
    // So we play note = rootNote + sliceIndex + 1
    unsigned char note = (unsigned char)(rootNote + sliceIndex + 1);

    playChannel_ = channel;
    PlayerMixer *mixer = PlayerMixer::GetInstance();
    mixer->StartInstrument(playChannel_, instr, note, true);
    playing_ = true;

    // Restore end
    if (endVar) endVar->SetInt(savedEnd);
}

void SampleEditorView::playFromStart() {
    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (!instr || instr->GetType() != IT_SAMPLE) return;

    SampleInstrument *si = (SampleInstrument *)instr;

    // Force oneshot mode — plays from SIP_START to SIP_END
    // (render thread reads loopMode_ live, so we must leave it set)
    Variable *loopMode = si->FindVariable(SIP_LOOPMODE);
    if (loopMode) loopMode->SetInt(SILM_ONESHOT);

    Variable *rootVar = si->FindVariable(SIP_ROOTNOTE);
    int rootNote = rootVar ? rootVar->GetInt() : 60;

    playChannel_ = viewData_->songX_;
    PlayerMixer *mixer = PlayerMixer::GetInstance();
    mixer->StartInstrument(playChannel_, instr, (unsigned char)rootNote, true);
    playing_ = true;
}

void SampleEditorView::stopPlayback() {
    if (!playing_) return;
    PlayerMixer *mixer = PlayerMixer::GetInstance();
    mixer->StopInstrument(playChannel_);
    playing_ = false;
    isDirty_ = true;
}

bool SampleEditorView::isPlaying() {
    if (!playing_) return false;
    // Check if playback has naturally finished
    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    if (!instr || instr->GetType() != IT_SAMPLE) {
        playing_ = false;
        return false;
    }
    SampleInstrument *si = (SampleInstrument *)instr;
    if (si->IsPlaybackFinished(playChannel_)) {
        playing_ = false;
        return false;
    }
    return true;
}

void SampleEditorView::getColorForDef(ColorDefinition cd,
                                       unsigned short &r, unsigned short &g,
                                       unsigned short &b) {
    switch (cd) {
    case CD_BACKGROUND:
        r = 0x1D; g = 0x0A; b = 0x1F;
        break;
    case CD_NORMAL:
        r = 0xF5; g = 0xEB; b = 0xFF;
        break;
    case CD_HILITE1:
        r = 0xB7; g = 0x50; b = 0xD1;
        break;
    case CD_HILITE2:
        r = 0xDB; g = 0x33; b = 0xDB;
        break;
    case CD_CURSOR:
        r = 0xFF; g = 0x00; b = 0x8C;
        break;
    default:
        r = 0xF5; g = 0xEB; b = 0xFF;
        break;
    }
}

void SampleEditorView::drawWaveformGraphical() {
    short *buf = getSampleBuffer();
    int totalSize = getSampleSize();
    if (!buf || totalSize <= 0) return;

    SDLGUIWindowImp *imp = (SDLGUIWindowImp *)w_.GetImpWindow();
    if (!imp) return;

    int viewRange = viewEnd_ - viewStart_;
    if (viewRange <= 0) return;

    int centerY = WAVE_PX_Y + WAVE_PX_H / 2;
    int halfH = WAVE_PX_H / 2;

    // Clear waveform area with background color
    unsigned short bgr, bgg, bgb;
    getColorForDef(CD_BACKGROUND, bgr, bgg, bgb);
    GUIColor bgColor(bgr, bgg, bgb);
    imp->SetColor(bgColor);
    GUIRect bgRect(WAVE_PX_X, WAVE_PX_Y, WAVE_PX_X + WAVE_PX_W, WAVE_PX_Y + WAVE_PX_H);
    imp->DrawRect(bgRect);

    // Draw cursor background first (so waveform draws on top)
    {
        unsigned short ccr, ccg, ccb;
        getColorForDef(CD_CURSOR, ccr, ccg, ccb);
        GUIColor cursorBgColor(ccr / 4, ccg / 4, ccb / 4);
        imp->SetColor(cursorBgColor);
        GUIRect cursorBg(WAVE_PX_X + cursorX_, WAVE_PX_Y,
                         WAVE_PX_X + cursorX_ + 3, WAVE_PX_Y + WAVE_PX_H);
        imp->DrawRect(cursorBg);
    }

    // Draw center line (dimmed)
    unsigned short cr, cg, cb;
    getColorForDef(CD_HILITE1, cr, cg, cb);
    GUIColor centerLineColor(cr / 3, cg / 3, cb / 3);
    imp->SetColor(centerLineColor);
    GUIRect centerLine(WAVE_PX_X, centerY, WAVE_PX_X + WAVE_PX_W, centerY + 1);
    imp->DrawRect(centerLine);

    // Pre-compute cursor color
    unsigned short ccr, ccg, ccb;
    getColorForDef(CD_CURSOR, ccr, ccg, ccb);
    GUIColor cursorColor(ccr, ccg, ccb);

    unsigned short wr, wg, wb;
    getColorForDef(CD_HILITE1, wr, wg, wb);
    GUIColor waveColor(wr, wg, wb);
    // Dimmed waveform color for regions outside start/end
    GUIColor waveDimColor(wr / 3, wg / 3, wb / 3);

    // Get start/end points from instrument for visual dimming
    int instrStart = 0;
    int instrEnd = totalSize;
    {
        int instrIdx = viewData_->currentInstrument_;
        InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
        I_Instrument *instr = bank->GetInstrument(instrIdx);
        if (instr && instr->GetType() == IT_SAMPLE) {
            SampleInstrument *si = (SampleInstrument *)instr;
            Variable *startVar = si->FindVariable(SIP_START);
            Variable *endVar = si->FindVariable(SIP_END);
            if (startVar) instrStart = startVar->GetInt();
            if (endVar) instrEnd = endVar->GetInt();
            if (instrEnd <= 0) instrEnd = totalSize;
        }
    }

    // Draw waveform column by column
    for (int col = 0; col < WAVE_PX_W; col++) {
        int sampleStart = viewStart_ + (int)((long long)col * viewRange / WAVE_PX_W);
        int sampleEnd = viewStart_ + (int)((long long)(col + 1) * viewRange / WAVE_PX_W);
        if (sampleEnd > totalSize) sampleEnd = totalSize;
        if (sampleStart >= totalSize) sampleStart = totalSize - 1;
        if (sampleStart < 0) sampleStart = 0;

        // Find min/max in this column's sample range
        short minVal = 0, maxVal = 0;
        for (int s = sampleStart; s < sampleEnd; s++) {
            if (buf[s] < minVal) minVal = buf[s];
            if (buf[s] > maxVal) maxVal = buf[s];
        }

        // Convert to pixel Y positions
        int topY = centerY - (int)((long long)maxVal * halfH / 32768);
        int botY = centerY - (int)((long long)minVal * halfH / 32768);

        if (topY < WAVE_PX_Y) topY = WAVE_PX_Y;
        if (botY > WAVE_PX_Y + WAVE_PX_H) botY = WAVE_PX_Y + WAVE_PX_H;
        if (topY > botY) topY = botY;

        // Ensure at least 1 pixel tall
        if (botY - topY < 1 && (maxVal != 0 || minVal != 0)) {
            botY = topY + 1;
        }

        bool isCursor = (col >= cursorX_ && col < cursorX_ + 3);

        // Determine if this column is outside the start/end range
        bool isOutside = (sampleStart < instrStart || sampleStart >= instrEnd);

        if (isCursor) {
            imp->SetColor(cursorColor);
        } else if (isOutside) {
            imp->SetColor(waveDimColor);
        } else {
            imp->SetColor(waveColor);
        }

        if (botY > topY) {
            GUIRect bar(WAVE_PX_X + col, topY,
                        WAVE_PX_X + col + 1, botY);
            imp->DrawRect(bar);
        }
    }

    // Draw start/end boundary markers as vertical lines
    if (instrStart > 0) {
        int sx = (int)((long long)(instrStart - viewStart_) * WAVE_PX_W / viewRange);
        if (sx >= 0 && sx < WAVE_PX_W) {
            GUIColor startColor(0x00, 0xFF, 0x00);
            imp->SetColor(startColor);
            GUIRect startLine(WAVE_PX_X + sx, WAVE_PX_Y,
                              WAVE_PX_X + sx + 1, WAVE_PX_Y + WAVE_PX_H);
            imp->DrawRect(startLine);
        }
    }
    if (instrEnd < totalSize) {
        int ex = (int)((long long)(instrEnd - viewStart_) * WAVE_PX_W / viewRange);
        if (ex >= 0 && ex < WAVE_PX_W) {
            GUIColor endColor(0xFF, 0x00, 0x00);
            imp->SetColor(endColor);
            GUIRect endLine(WAVE_PX_X + ex, WAVE_PX_Y,
                            WAVE_PX_X + ex + 1, WAVE_PX_Y + WAVE_PX_H);
            imp->DrawRect(endLine);
        }
    }
}

void SampleEditorView::drawSliceMarkersGraphical() {
    if (sliceCount_ <= 0) return;

    SDLGUIWindowImp *imp = (SDLGUIWindowImp *)w_.GetImpWindow();
    if (!imp) return;

    for (int i = 0; i < sliceCount_; i++) {
        int screenX = sampleToScreen(slicePoints_[i]);
        if (screenX < 0 || screenX >= WAVE_PX_W) continue;

        bool isSelected = (i == currentSlice_ && mode_ == MODE_SLICE);

        unsigned short sr, sg, sb;
        if (isSelected) {
            getColorForDef(CD_CURSOR, sr, sg, sb);
        } else {
            getColorForDef(CD_HILITE2, sr, sg, sb);
        }
        GUIColor sliceColor(sr, sg, sb);
        imp->SetColor(sliceColor);

        int x = WAVE_PX_X + screenX;
        int width = isSelected ? 2 : 1;
        GUIRect sliceLine(x, WAVE_PX_Y, x + width, WAVE_PX_Y + WAVE_PX_H);
        imp->DrawRect(sliceLine);

        // Draw slice number label at the top of the waveform area
        char label[4];
        sprintf(label, "%X", i);
        GUITextProperties props;
        if (isSelected) {
            SetColor(CD_CURSOR);
        } else {
            SetColor(CD_HILITE2);
        }
        // Convert pixel position back to character column for the label
        int charCol = (WAVE_PX_X + screenX) / 8;
        if (charCol >= 0 && charCol < 80) {
            DrawString(charCol, 1, label, props);
        }
    }
}

void SampleEditorView::drawInfo() {
    GUITextProperties props;
    GUIPoint pos = GetTitlePosition();

    SetColor(CD_NORMAL);

    // Title
    char title[80];
    int instrIdx = viewData_->currentInstrument_;

    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);
    const char *name = "---";
    if (instr && instr->GetType() == IT_SAMPLE) {
        name = instr->GetName();
    }

    sprintf(title, "Sample Editor: I%02X [%s]", instrIdx, name ? name : "---");
    DrawString(pos._x, pos._y, title, props);

    // Slice info on INFO_Y line (only in slice mode)
    if (mode_ == MODE_SLICE) {
        char info[80];
        sprintf(info, "Slice %d/%d",
                currentSlice_ + 1, sliceCount_);
        DrawString(1, INFO_Y, info, props);
    }

    // Cursor position - far right, just below waveform (row 24)
    int cursorSample = screenToSample(cursorX_);
    char cursorStr[32];
    sprintf(cursorStr, "Cursor:%d", cursorSample);
    int len = (int)strlen(cursorStr);
    DrawString(68 - len, 24, cursorStr, props);
}

void SampleEditorView::drawHelp() {
    GUITextProperties props;

    SetColor(CD_NORMAL);

    if (mode_ == MODE_NAVIGATE) {
        DrawString(1, 24,         "Select:Open Slice Mode", props);
        DrawString(1, HELP_Y,     "L4:Start Point   R4:End Point", props);
    } else {
        DrawString(1, 24,         "Select:Open Trim Mode", props);
        DrawString(1, HELP_Y,     "A+L/R:Slice Navigation", props);
        DrawString(1, HELP_Y + 1, "A:Add Slice   B+A:Del", props);
    }

    // "L to exit" at bottom right
    DrawString(60, HELP_Y + 2, "L to exit", props);
}

void SampleEditorView::DrawView() {
    Clear();

    short *buf = getSampleBuffer();
    int totalSize = getSampleSize();

    if (!buf || totalSize <= 0) {
        GUITextProperties props;
        GUIPoint pos = GetTitlePosition();
        SetColor(CD_NORMAL);
        DrawString(pos._x, pos._y, "Sample Editor - No Sample Loaded", props);
        DrawString(pos._x, pos._y + 2, "Assign a sample in Instrument view", props);
        DrawString(pos._x, pos._y + 3, "then press edit to open editor", props);
        return;
    }

    // Text elements go into the text buffer
    // The waveform area is drawn as pixels in DrawGraphics()
    drawInfo();
    drawHelp();
}

void SampleEditorView::DrawGraphics() {
    short *buf = getSampleBuffer();
    int totalSize = getSampleSize();
    if (!buf || totalSize <= 0) return;

    drawWaveformGraphical();
    drawSliceMarkersGraphical();

    // Draw playhead if playing
    if (isPlaying()) {
        int instrIdx = viewData_->currentInstrument_;
        InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
        I_Instrument *instr = bank->GetInstrument(instrIdx);
        if (instr && instr->GetType() == IT_SAMPLE) {
            SampleInstrument *si = (SampleInstrument *)instr;
            int pos = (int)si->GetPlaybackPosition(playChannel_);
            int viewRange = viewEnd_ - viewStart_;
            if (viewRange <= 0) viewRange = 1;
            int sx = (int)((long long)(pos - viewStart_) * WAVE_PX_W / viewRange);
            if (sx >= 0 && sx < WAVE_PX_W) {
                SDLGUIWindowImp *imp = (SDLGUIWindowImp *)w_.GetImpWindow();
                if (imp) {
                    GUIColor playColor(0xFF, 0xFF, 0x00); // Yellow playhead
                    imp->SetColor(playColor);
                    GUIRect line(WAVE_PX_X + sx, WAVE_PX_Y,
                                 WAVE_PX_X + sx + 2, WAVE_PX_Y + WAVE_PX_H);
                    imp->DrawRect(line);
                }
            }
        }
    }
}

void SampleEditorView::OnPlayerUpdate(PlayerEventType, unsigned int tick) {
    // No note display in sample editor
}

void SampleEditorView::OnFocus() {
    stopPlayback();
    int instrIdx = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(instrIdx);

    sliceCount_ = 0;
    currentSlice_ = -1;

    if (instr && instr->GetType() == IT_SAMPLE) {
        SampleInstrument *si = (SampleInstrument *)instr;
        sliceCount_ = si->GetSliceCount();
        for (int i = 0; i < sliceCount_ && i < MAX_SLICE_COUNT; i++) {
            slicePoints_[i] = si->GetSlicePoint(i);
        }
    }

    int totalSize = getSampleSize();
    viewStart_ = 0;
    viewEnd_ = (totalSize > 0) ? totalSize : 0;
    zoomLevel_ = 0;
    cursorX_ = WAVE_PX_W / 2;
    mode_ = MODE_NAVIGATE;
}

void SampleEditorView::moveCursorAndAutoScroll(int delta) {
    cursorX_ += delta;

    // Calculate how many samples one cursor step (|delta| pixels) represents
    int range = viewEnd_ - viewStart_;
    if (range < 1) range = 1;
    int absDelta = delta < 0 ? -delta : delta;
    int sampleStep = (int)((long long)absDelta * range / WAVE_PX_W);
    if (sampleStep < 1) sampleStep = 1;

    if (cursorX_ < 0) {
        cursorX_ = 0;
        // Auto-scroll left at same speed as cursor movement
        if (viewStart_ > 0) {
            viewStart_ -= sampleStep;
            viewEnd_ -= sampleStep;
            if (viewStart_ < 0) {
                viewStart_ = 0;
                viewEnd_ = range;
            }
        }
    }
    if (cursorX_ >= WAVE_PX_W - 3) {
        cursorX_ = WAVE_PX_W - 4;
        // Auto-scroll right at same speed as cursor movement
        int totalSize = getSampleSize();
        if (viewEnd_ < totalSize) {
            viewStart_ += sampleStep;
            viewEnd_ += sampleStep;
            if (viewEnd_ > totalSize) {
                viewEnd_ = totalSize;
                viewStart_ = viewEnd_ - range;
                if (viewStart_ < 0) viewStart_ = 0;
            }
        }
    }
    isDirty_ = true;
}

void SampleEditorView::processNormalButtonMask(unsigned short mask) {

    // SELECT: toggle mode
    if (mask == EPBM_SELECT) {
        mode_ = (mode_ == MODE_NAVIGATE) ? MODE_SLICE : MODE_NAVIGATE;
        isDirty_ = true;
        return;
    }

    // L button: back to instrument view
    if (mask & EPBM_L) {
        stopPlayback();
        ViewType vt = VT_INSTRUMENT;
        ViewEvent ve(VET_SWITCH_VIEW, &vt);
        SetChanged();
        NotifyObservers(&ve);
        return;
    }

    // PGBACK: set loop start point (navigate/trim mode only)
    if ((mask & EPBM_PGBACK) && mode_ == MODE_NAVIGATE) {
        setStartPoint();
        return;
    }

    // PGFWD: set loop end point (navigate/trim mode only)
    if ((mask & EPBM_PGFWD) && mode_ == MODE_NAVIGATE) {
        setEndPoint();
        return;
    }

    // START alone: toggle play/stop
    if (mask == EPBM_START) {
        if (isPlaying()) {
            stopPlayback();
        } else {
            playFromStart();
        }
        return;
    }

    // B modifier (scroll, remove slice)
    if (mask & EPBM_B) {
        if (mask & EPBM_LEFT) {
            scrollLeft();
            return;
        }
        if (mask & EPBM_RIGHT) {
            scrollRight();
            return;
        }
        if (mask & EPBM_A) {
            if (mode_ == MODE_SLICE) {
                removeSlice();
                return;
            }
        }
    }

    // A modifier in slice mode: jump between slices and play them
    if (mode_ == MODE_SLICE && (mask & EPBM_A)) {
        if (mask & EPBM_LEFT) {
            moveSliceCursor(-1);
            if (currentSlice_ >= 0) playSlice(currentSlice_);
            return;
        }
        if (mask & EPBM_RIGHT) {
            moveSliceCursor(1);
            if (currentSlice_ >= 0) playSlice(currentSlice_);
            return;
        }
        // A alone in slice mode: add slice at cursor
        if (mask == EPBM_A) {
            addSlice();
            return;
        }
    }

    // A modifier in nav mode: zoom
    if (mode_ == MODE_NAVIGATE && (mask & EPBM_A)) {
        if (mask & EPBM_LEFT) {
            zoomOut();
            return;
        }
        if (mask & EPBM_RIGHT) {
            zoomIn();
            return;
        }
    }

    // UP/DOWN: zoom in both modes
    if (mask & EPBM_UP) {
        zoomIn();
        return;
    }
    if (mask & EPBM_DOWN) {
        zoomOut();
        return;
    }

    // Unmodified L/R: move cursor in both modes, with auto-scroll
    if (mask & EPBM_LEFT) {
        moveCursorAndAutoScroll(-8);
    }
    if (mask & EPBM_RIGHT) {
        moveCursorAndAutoScroll(8);
    }
}

void SampleEditorView::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed) return;
    processNormalButtonMask(mask);
}
