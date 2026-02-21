#include "LoadThemeDialog.h"
#include "System/Console/Trace.h"
#include <algorithm>
#include <cstring>

static char *themeButtonText[2] = {(char *)"Load", (char *)"Cancel"};

LoadThemeDialog::LoadThemeDialog(View &view)
    : ModalView(view), content_(true) {
    topIndex_ = 0;
    currentFile_ = 0;
    selected_ = 0;
}

LoadThemeDialog::~LoadThemeDialog() {}

void LoadThemeDialog::loadFileList() {
    content_.Empty();

    Path themesDir("root:themes");
    std::string dirPath = themesDir.GetPath();

    I_Dir *dir = FileSystem::GetInstance()->Open(dirPath.c_str());
    if (dir) {
        dir->GetContent("*");
        dir->Sort();

        IteratorPtr<Path> it(dir->GetIterator());
        for (it->Begin(); !it->IsDone(); it->Next()) {
            Path &current = it->CurrentItem();
            if (current.IsFile()) {
                std::string name = current.GetName();
                // Only show .theme files
                if (name.size() > 6 &&
                    name.substr(name.size() - 6) == ".theme") {
                    Path *p = new Path(current);
                    content_.Insert(p);
                }
            }
        }
        delete dir;
    }
}

void LoadThemeDialog::DrawView() {
    SetWindow(THEME_LIST_WIDTH, THEME_LIST_SIZE + 3);

    GUITextProperties props;
    SetColor(CD_NORMAL);
    View::EnableNotification();

    // Title
    DrawString(2, 0, "Load Theme", props);

    // Clamp scroll
    if (currentFile_ < topIndex_) {
        topIndex_ = currentFile_;
    }
    if (currentFile_ >= topIndex_ + THEME_LIST_SIZE) {
        topIndex_ = currentFile_ - THEME_LIST_SIZE + 1;
    }

    int y = 1;
    int count = 0;
    char buffer[256];

    IteratorPtr<Path> it(content_.GetIterator());
    for (it->Begin(); !it->IsDone(); it->Next()) {
        if (count >= topIndex_ && count < topIndex_ + THEME_LIST_SIZE) {
            Path &current = it->CurrentItem();
            std::string name = current.GetName();
            // Strip .theme extension for display
            if (name.size() > 6) {
                name = name.substr(0, name.size() - 6);
            }

            if (count == currentFile_ && selected_ == 0) {
                SetColor(CD_HILITE2);
                props.invert_ = true;
            } else {
                SetColor(CD_NORMAL);
                props.invert_ = false;
            }
            snprintf(buffer, sizeof(buffer), " %s", name.c_str());
            buffer[THEME_LIST_WIDTH - 1] = 0;
            DrawString(1, y, buffer, props);
            y++;
        }
        count++;
    }

    // If no themes found
    if (count == 0) {
        props.invert_ = false;
        SetColor(CD_NORMAL);
        DrawString(2, 2, "No themes found", props);
    }

    // Buttons
    y = THEME_LIST_SIZE + 2;
    int offset = THEME_LIST_WIDTH / 3;
    SetColor(CD_NORMAL);

    for (int i = 0; i < 2; i++) {
        const char *text = themeButtonText[i];
        int x = offset * (i + 1) - (int)strlen(text) / 2;
        props.invert_ = (selected_ == i + 1);
        DrawString(x, y, text, props);
    }
}

void LoadThemeDialog::OnPlayerUpdate(PlayerEventType, unsigned int) {}

void LoadThemeDialog::OnFocus() {
    loadFileList();
    currentFile_ = 0;
    topIndex_ = 0;
    selected_ = 0;
}

void LoadThemeDialog::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed) return;

    if (mask & EPBM_B) {
        // B+UP/DOWN = page scroll
        if (mask & EPBM_UP) {
            currentFile_ -= THEME_LIST_SIZE;
            if (currentFile_ < 0) currentFile_ = 0;
            isDirty_ = true;
        }
        if (mask & EPBM_DOWN) {
            int size = content_.Size();
            currentFile_ += THEME_LIST_SIZE;
            if (currentFile_ >= size) currentFile_ = size - 1;
            if (currentFile_ < 0) currentFile_ = 0;
            isDirty_ = true;
        }
        return;
    }

    if (mask & EPBM_A) {
        if (selected_ == 0) {
            // Select from list — move to Load button
            selected_ = 1;
            isDirty_ = true;
        } else if (selected_ == 1) {
            // Load — find the selected file
            int count = 0;
            IteratorPtr<Path> it(content_.GetIterator());
            for (it->Begin(); !it->IsDone(); it->Next()) {
                if (count == currentFile_) {
                    selection_ = it->CurrentItem().GetPath();
                    EndModal(1);
                    return;
                }
                count++;
            }
            // Nothing selected
            EndModal(0);
        } else if (selected_ == 2) {
            // Cancel
            EndModal(0);
        }
        return;
    }

    // Navigation
    int size = content_.Size();

    if (mask == EPBM_UP) {
        if (selected_ == 0 && size > 0) {
            currentFile_--;
            if (currentFile_ < 0) currentFile_ = size - 1;
        }
        isDirty_ = true;
    }
    if (mask == EPBM_DOWN) {
        if (selected_ == 0 && size > 0) {
            currentFile_++;
            if (currentFile_ >= size) currentFile_ = 0;
        }
        isDirty_ = true;
    }
    if (mask == EPBM_LEFT) {
        if (selected_ > 0) {
            selected_--;
        } else {
            selected_ = 2;
        }
        isDirty_ = true;
    }
    if (mask == EPBM_RIGHT) {
        if (selected_ < 2) {
            selected_++;
        } else {
            selected_ = 0;
        }
        isDirty_ = true;
    }
}

std::string LoadThemeDialog::GetSelection() {
    return selection_;
}
