#ifndef _IMPORT_SOUNDFONT_DIALOG_H_
#define _IMPORT_SOUNDFONT_DIALOG_H_

#include "Application/Views/BaseClasses/ModalView.h"
#include "Foundation/T_SimpleList.h"
#include "System/FileSystem/FileSystem.h"
#include <string>
#include <vector>

class ImportSoundFontDialog : public ModalView {
public:
    ImportSoundFontDialog(View &view);
    virtual ~ImportSoundFontDialog();

    virtual void DrawView();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int currentTick);
    virtual void OnFocus();
    virtual void ProcessButtonMask(unsigned short mask, bool pressed);

protected:
    void scanForSF2Files(Path folder);
    void warpToNext(int dir);
    void selectFile();
    void loadPresetsForCurrent();
    void warpPreset(int dir);
    void selectPreset();

private:
    // SF2 file list
    T_SimpleList<Path> sf2List_;
    int currentFile_;
    int topIndex_;
    int toInstr_;

    // Preset list for currently highlighted SF2
    std::vector<std::string> presetNames_;
    int currentPreset_;
    int presetTopIndex_;

    // State: are we browsing files or presets?
    enum DialogMode {
        MODE_FILES,
        MODE_PRESETS
    };
    DialogMode mode_;

    // Cache the loaded bank ID for preset enumeration
    int tempBankID_;
    char tempSF2Path_[256];

    static Path sampleLib_;
    static bool initStatic_;
};

#endif
