#ifndef _LOAD_THEME_DIALOG_H_
#define _LOAD_THEME_DIALOG_H_

#include "Application/Views/BaseClasses/ModalView.h"
#include "System/FileSystem/FileSystem.h"
#include <string>

#define THEME_LIST_SIZE 15
#define THEME_LIST_WIDTH 28

class LoadThemeDialog : public ModalView {
public:
    LoadThemeDialog(View &view);
    virtual ~LoadThemeDialog();

    virtual void DrawView();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int currentTick);
    virtual void OnFocus();
    virtual void ProcessButtonMask(unsigned short mask, bool pressed);

    std::string GetSelection();

private:
    void loadFileList();

    T_SimpleList<Path> content_;
    int topIndex_;
    int currentFile_;
    int selected_;  // 0 = list, 1 = Load, 2 = Cancel
    std::string selection_;
};

#endif
