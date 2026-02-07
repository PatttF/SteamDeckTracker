#include "ImportSoundFontDialog.h"
#include "Application/Instruments/InstrumentBank.h"
#include "Application/Instruments/SoundFontInstrument.h"
#include "Application/Instruments/SoundFontManager.h"
#include "Application/Instruments/SamplePool.h"
#include "System/Console/Trace.h"
#include "Externals/Soundfont/ENAB.H"
#include "Externals/Soundfont/HYDRA.H"
#include <cstring>

#define LIST_SIZE 15
#define LIST_WIDTH 28

bool ImportSoundFontDialog::initStatic_ = false;
Path ImportSoundFontDialog::sampleLib_("");

ImportSoundFontDialog::ImportSoundFontDialog(View &view) : ModalView(view) {
    currentFile_ = 0;
    topIndex_ = 0;
    currentPreset_ = 0;
    presetTopIndex_ = 0;
    mode_ = MODE_FILES;
    tempBankID_ = -1;
    tempSF2Path_[0] = '\0';
    toInstr_ = view.viewData_->currentInstrument_;

    if (!initStatic_) {
        const char *slpath = SamplePool::GetInstance()->GetSampleLib();
        sampleLib_ = Path(slpath);
        initStatic_ = true;
    }
}

ImportSoundFontDialog::~ImportSoundFontDialog() {
    // Unload any temp bank we loaded for preset preview
    if (tempBankID_ >= 0) {
        SoundFontManager::GetInstance()->UnloadBank(tempBankID_);
        tempBankID_ = -1;
    }

    // Clean up file list
    IteratorPtr<Path> it(sf2List_.GetIterator());
    for (it->Begin(); !it->IsDone(); it->Next()) {
        delete &it->CurrentItem();
    }
    sf2List_.Empty();
}

void ImportSoundFontDialog::scanForSF2Files(Path *folder) {
    // Clear existing list
    IteratorPtr<Path> it(sf2List_.GetIterator());
    for (it->Begin(); !it->IsDone(); it->Next()) {
        delete &it->CurrentItem();
    }
    sf2List_.Empty();
    currentFile_ = 0;
    topIndex_ = 0;

    if (!folder) return;

    I_Dir *dir = FileSystem::GetInstance()->Open(folder->GetPath().c_str());
    if (!dir) return;

    dir->GetContent("*");
    dir->Sort();

    IteratorPtr<Path> dit(dir->GetIterator());

    // First pass: add directories (for navigation)
    for (dit->Begin(); !dit->IsDone(); dit->Next()) {
        Path &current = dit->CurrentItem();
        if (current.IsDirectory()) {
            if (current.GetName().substr(0, 1) != ".") {
                Path *p = new Path(current);
                sf2List_.Insert(p);
            }
        }
    }

    // Add ".." for navigating up (only if not at samplelib root)
    if (folder->GetPath() != sampleLib_.GetPath()) {
        Path *parent = new Path(folder->GetParent());
        sf2List_.Insert(parent);
    }

    // Second pass: add .sf2 files
    for (dit->Begin(); !dit->IsDone(); dit->Next()) {
        Path &current = dit->CurrentItem();
        if (!current.IsDirectory()) {
            if (current.Matches("*.sf2") || current.Matches("*.SF2")) {
                Path *p = new Path(current);
                sf2List_.Insert(p);
            }
        }
    }

    delete dir;
}

void ImportSoundFontDialog::OnFocus() {
    Path current(sampleLib_);
    scanForSF2Files(&current);
    isDirty_ = true;
}

void ImportSoundFontDialog::OnPlayerUpdate(PlayerEventType, unsigned int) {
}

void ImportSoundFontDialog::DrawView() {
    SetWindow(LIST_WIDTH, LIST_SIZE + 3);

    GUITextProperties props;
    SetColor(CD_NORMAL);

    if (mode_ == MODE_FILES) {
        // Draw SF2 file list
        int x = 1;
        int y = 0;

        // Title
        DrawString(x, y, "Select SoundFont:", props);
        y = 1;

        int count = 0;
        IteratorPtr<Path> it(sf2List_.GetIterator());

        for (it->Begin(); !it->IsDone(); it->Next()) {
            if (count >= topIndex_ && count < topIndex_ + LIST_SIZE) {
                Path &current = it->CurrentItem();
                const std::string name = current.GetName();

                if (count == currentFile_) {
                    SetColor(CD_HILITE2);
                    props.invert_ = true;
                } else {
                    SetColor(CD_NORMAL);
                    props.invert_ = false;
                }

                char buffer[LIST_WIDTH + 1];
                if (current.IsDirectory()) {
                    snprintf(buffer, sizeof(buffer), "[%s]", name.c_str());
                } else {
                    strncpy(buffer, name.c_str(), LIST_WIDTH);
                    buffer[LIST_WIDTH] = '\0';
                }
                buffer[LIST_WIDTH - 1] = '\0';
                DrawString(x, y, buffer, props);
                y++;
            }
            count++;
        }

        // Footer
        SetColor(CD_NORMAL);
        props.invert_ = false;
        y = LIST_SIZE + 2;
        DrawString(1, y, "A:select  B:exit", props);

    } else {
        // MODE_PRESETS - Draw preset list
        int x = 1;
        int y = 0;

        // Title: show the selected SF2 filename
        const char *slash = strrchr(tempSF2Path_, '/');
        const char *fname = slash ? slash + 1 : tempSF2Path_;
        char title[LIST_WIDTH + 1];
        snprintf(title, sizeof(title), "Presets: %s", fname);
        DrawString(x, y, title, props);
        y = 1;

        int count = 0;
        for (int i = 0; i < (int)presetNames_.size(); i++) {
            if (count >= presetTopIndex_ && count < presetTopIndex_ + LIST_SIZE) {
                if (count == currentPreset_) {
                    SetColor(CD_HILITE2);
                    props.invert_ = true;
                } else {
                    SetColor(CD_NORMAL);
                    props.invert_ = false;
                }

                char buffer[LIST_WIDTH + 1];
                snprintf(buffer, sizeof(buffer), "%02d:%s", i, presetNames_[i].c_str());
                buffer[LIST_WIDTH - 1] = '\0';
                DrawString(x, y, buffer, props);
                y++;
            }
            count++;
        }

        // Footer
        SetColor(CD_NORMAL);
        props.invert_ = false;
        y = LIST_SIZE + 2;
        DrawString(1, y, "A:load  B:back", props);
    }
}

void ImportSoundFontDialog::warpToNext(int dir) {
    if (mode_ == MODE_FILES) {
        int size = sf2List_.Size();
        if (size == 0) return;
        currentFile_ += dir;
        if (currentFile_ < 0) currentFile_ = 0;
        if (currentFile_ >= size) currentFile_ = size - 1;
        if (currentFile_ < topIndex_) topIndex_ = currentFile_;
        if (currentFile_ >= topIndex_ + LIST_SIZE) topIndex_ = currentFile_ - LIST_SIZE + 1;
    } else {
        warpPreset(dir);
    }
    isDirty_ = true;
}

void ImportSoundFontDialog::warpPreset(int dir) {
    int size = (int)presetNames_.size();
    if (size == 0) return;
    currentPreset_ += dir;
    if (currentPreset_ < 0) currentPreset_ = 0;
    if (currentPreset_ >= size) currentPreset_ = size - 1;
    if (currentPreset_ < presetTopIndex_) presetTopIndex_ = currentPreset_;
    if (currentPreset_ >= presetTopIndex_ + LIST_SIZE) presetTopIndex_ = currentPreset_ - LIST_SIZE + 1;
}

void ImportSoundFontDialog::loadPresetsForCurrent() {
    presetNames_.clear();
    currentPreset_ = 0;
    presetTopIndex_ = 0;

    if (tempBankID_ < 0) return;

    WORD count = 0;
    SFPRESETHDRPTR pHeaders = sfGetPresetHdrs(tempBankID_, &count);
    if (!pHeaders || count == 0) return;

    for (int i = 0; i < count; i++) {
        presetNames_.push_back(std::string(pHeaders[i].achPresetName));
    }
}

void ImportSoundFontDialog::selectFile() {
    // Get the selected file
    IteratorPtr<Path> it(sf2List_.GetIterator());
    int count = 0;

    for (it->Begin(); !it->IsDone(); it->Next()) {
        if (count == currentFile_) {
            Path &selected = it->CurrentItem();

            if (selected.IsDirectory()) {
                // Navigate into directory
                scanForSF2Files(&selected);
                isDirty_ = true;
                return;
            }

            // It's an SF2 file - load it for preset browsing
            strncpy(tempSF2Path_, selected.GetPath().c_str(), sizeof(tempSF2Path_) - 1);
            tempSF2Path_[sizeof(tempSF2Path_) - 1] = '\0';

            // Unload previous temp bank if any
            if (tempBankID_ >= 0) {
                SoundFontManager::GetInstance()->UnloadBank(tempBankID_);
                tempBankID_ = -1;
            }

            // Load the bank to enumerate presets
            tempBankID_ = SoundFontManager::GetInstance()->LoadBank(tempSF2Path_);
            if (tempBankID_ < 0) {
                Trace::Error("ImportSoundFontDialog: Failed to load %s", tempSF2Path_);
                return;
            }

            loadPresetsForCurrent();

            if (presetNames_.empty()) {
                Trace::Error("ImportSoundFontDialog: No presets in %s", tempSF2Path_);
                return;
            }

            // Switch to preset selection mode
            mode_ = MODE_PRESETS;
            isDirty_ = true;
            return;
        }
        count++;
    }
}

void ImportSoundFontDialog::selectPreset() {
    if (currentPreset_ < 0 || currentPreset_ >= (int)presetNames_.size()) return;
    if (tempBankID_ < 0) return;

    // Apply to the instrument
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(toInstr_);

    if (instr->GetType() == IT_SOUNDFONT) {
        SoundFontInstrument *sfi = (SoundFontInstrument *)instr;
        sfi->SetSF2File(tempSF2Path_);
        sfi->SelectPreset(currentPreset_);
        Trace::Debug("ImportSoundFontDialog: Loaded preset %d from %s", currentPreset_, tempSF2Path_);
    }

    EndModal(0);
}

void ImportSoundFontDialog::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed) return;

    if (mask & EPBM_B) {
        // B+direction for page scrolling
        if (mask & EPBM_UP) {
            warpToNext(-LIST_SIZE);
        } else if (mask & EPBM_DOWN) {
            warpToNext(LIST_SIZE);
        } else if (mask == EPBM_B) {
            // Plain B: go back or exit
            if (mode_ == MODE_PRESETS) {
                // Unload temp bank when going back to file list
                if (tempBankID_ >= 0) {
                    SoundFontManager::GetInstance()->UnloadBank(tempBankID_);
                    tempBankID_ = -1;
                }
                mode_ = MODE_FILES;
                isDirty_ = true;
            } else {
                EndModal(0);
            }
        }
    } else {
        // No B modifier
        if (mask == EPBM_UP) {
            warpToNext(-1);
        } else if (mask == EPBM_DOWN) {
            warpToNext(1);
        } else if (mask == EPBM_A) {
            if (mode_ == MODE_FILES) {
                selectFile();
            } else {
                selectPreset();
            }
        }
    }
}
