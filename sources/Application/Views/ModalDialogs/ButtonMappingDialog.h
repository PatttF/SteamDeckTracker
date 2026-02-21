#ifndef _BUTTON_MAPPING_DIALOG_H_
#define _BUTTON_MAPPING_DIALOG_H_

#include "Application/Views/BaseClasses/ModalView.h"
#include <string>

// Number of buttons we prompt the user to map
#define MAP_STEP_COUNT 12

class ButtonMappingDialog : public ModalView {
public:
    ButtonMappingDialog(View &view);
    virtual ~ButtonMappingDialog();

    virtual void DrawView();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int) {};
    virtual void OnFocus();
    virtual void ProcessButtonMask(unsigned short mask, bool pressed);

    // Called by the parent view callback to write the mapping file
    bool WriteMappingFile();

private:
    int currentStep_;          // which button we're currently prompting for
    bool waitingForRelease_;   // wait for all buttons released before capturing next
    std::string captured_[MAP_STEP_COUNT]; // captured source strings per step

    // Step definitions
    static const char *stepNames_[MAP_STEP_COUNT];
    static const char *stepDstUrls_[MAP_STEP_COUNT];
};

#endif
