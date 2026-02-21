
#include "SavePresetDialog.h"
#include "Application/Utils/KeyboardLayout.h"

static char *savePresetButtonText[SAVE_PRESET_BUTTONS] = {
    (char *)"Ok", (char *)"Cancel"};

#define SAVE_DIALOG_WIDTH 24

SavePresetDialog::SavePresetDialog(View &view)
    : ModalView(view) {}

SavePresetDialog::~SavePresetDialog() {}

void SavePresetDialog::moveCursor(int direction) {
    int newPos = currentChar_ + direction;
    if (newPos >= 0 && newPos < SAVE_PRESET_MAX_NAME) {
        currentChar_ = newPos;
        findCharacterInKeyboard(name_[currentChar_], keyboardRow_,
                                keyboardCol_);
    }
}

void SavePresetDialog::DrawView() {

    SetWindow(SAVE_DIALOG_WIDTH, keyboardMode_ ? 15 : 5);

    GUITextProperties props;

    SetColor(CD_NORMAL);

    // Draw title
    DrawString(2, 0, "Save Preset", props);

    // Draw name string
    int x = (SAVE_DIALOG_WIDTH - SAVE_PRESET_MAX_NAME) / 2;

    char buffer[2];
    buffer[1] = 0;
    for (int i = 0; i < SAVE_PRESET_MAX_NAME; i++) {
        props.invert_ = ((i == currentChar_) && (selected_ == 0));
        buffer[0] = name_[i];
        DrawString(x + i, 2, buffer, props);
    }

    // Draw keyboard if in keyboard mode
    if (keyboardMode_) {
        SetColor(CD_NORMAL);
        for (int row = 0; row < KEYBOARD_ROWS; row++) {
            const char *rowStr = keyboardLayout[row];
            int len = strlen(rowStr);
            int startX = (SAVE_DIALOG_WIDTH - len) / 2;

            if (row == SPACE_ROW) {
                props.invert_ =
                    (row == keyboardRow_ && isInSpaceSection(keyboardCol_));
                DrawString(startX, 4 + row, "[_]", props);

                props.invert_ =
                    (row == keyboardRow_ && isInBackSection(keyboardCol_));
                DrawString(startX + 4, 4 + row, "<-", props);

                props.invert_ =
                    (row == keyboardRow_ && isInDoneSection(keyboardCol_));
                DrawString(startX + 8, 4 + row, "OK", props);
            } else {
                for (int col = 0; col < len; col++) {
                    props.invert_ =
                        (row == keyboardRow_ && col == keyboardCol_);
                    buffer[0] = rowStr[col];
                    DrawString(startX + col, 4 + row, buffer, props);
                }
            }
        }
        props.invert_ = false;
        int xOffset = 0, yOffset = 13;
        DrawString(x + xOffset, yOffset, "A=input, B=erase", props);
        DrawString(x + xOffset, yOffset + 2, "L, R=move cursor", props);
        return;
    }

    // Draw buttons
    SetColor(CD_NORMAL);
    props.invert_ = false;

    int offset = SAVE_DIALOG_WIDTH / (SAVE_PRESET_BUTTONS + 1);

    for (int i = 0; i < SAVE_PRESET_BUTTONS; i++) {
        const char *text = savePresetButtonText[i];
        x = (offset * (i + 1) - strlen(text) / SAVE_PRESET_BUTTONS) - 2;
        props.invert_ = (selected_ == i + 1);
        DrawString(x, 4, text, props);
    }
    View::EnableNotification();
}

void SavePresetDialog::OnPlayerUpdate(PlayerEventType,
                                      unsigned int currentTick) {};

void SavePresetDialog::OnFocus() {
    selected_ = currentChar_ = 0;
    memset(name_, ' ', SAVE_PRESET_MAX_NAME + 1);
    lastChar_ = 'A';
    keyboardMode_ = false;
    keyboardRow_ = 2;
    keyboardCol_ = 0;
};

void SavePresetDialog::ProcessButtonMask(unsigned short mask, bool pressed) {

    if (!pressed)
        return;

    // Handle keyboard mode navigation
    if (keyboardMode_) {
        if (mask == EPBM_A) {
            char ch = getKeyAtPosition(keyboardRow_, keyboardCol_);
            if (ch == '\b') {
                if (currentChar_ > 0) {
                    currentChar_--;
                    name_[currentChar_] = ' ';
                }
            } else if (ch == '\r') {
                keyboardMode_ = false;
                isDirty_ = true;
                return;
            } else if (ch != '\0') {
                name_[currentChar_] = ch;
                lastChar_ = ch;
                if (currentChar_ < SAVE_PRESET_MAX_NAME - 1) {
                    currentChar_++;
                    findCharacterInKeyboard(name_[currentChar_], keyboardRow_,
                                            keyboardCol_);
                }
            }
            isDirty_ = true;
            return;
        } else if (mask == EPBM_B) {
            if (currentChar_ > 0) {
                currentChar_--;
                name_[currentChar_] = ' ';
                isDirty_ = true;
            }
            return;
        } else if (mask == EPBM_L) {
            moveCursor(-1);
            isDirty_ = true;
            return;
        } else if (mask == EPBM_R) {
            moveCursor(1);
            isDirty_ = true;
            return;
        } else if (mask == EPBM_UP) {
            keyboardRow_ = (keyboardRow_ - 1 + KEYBOARD_ROWS) % KEYBOARD_ROWS;
            clampKeyboardColumn(keyboardRow_, keyboardCol_);
            isDirty_ = true;
            return;
        } else if (mask == EPBM_DOWN) {
            keyboardRow_ = (keyboardRow_ + 1) % KEYBOARD_ROWS;
            clampKeyboardColumn(keyboardRow_, keyboardCol_);
            isDirty_ = true;
            return;
        } else if (mask == EPBM_LEFT) {
            cycleKeyboardColumn(keyboardRow_, -1, keyboardCol_);
            isDirty_ = true;
            return;
        } else if (mask == EPBM_RIGHT) {
            cycleKeyboardColumn(keyboardRow_, 1, keyboardCol_);
            isDirty_ = true;
            return;
        } else if (mask == EPBM_START) {
            keyboardMode_ = false;
            isDirty_ = true;
            return;
        }
        return;
    } else if (mask & EPBM_A) {
        if (mask == EPBM_A) {
            switch (selected_) {
            case 0:
                // Toggle keyboard mode on name field
                keyboardMode_ = !keyboardMode_;
                if (keyboardMode_) {
                    findCharacterInKeyboard(name_[currentChar_], keyboardRow_,
                                            keyboardCol_);
                }
                isDirty_ = true;
                break;
            case 1:
                // Ok - check name is not blank
                {
                    bool blank = true;
                    for (int i = 0; i < SAVE_PRESET_MAX_NAME; i++) {
                        if (name_[i] != ' ') { blank = false; break; }
                    }
                    if (blank) {
                        // Don't allow blank name
                    } else {
                        EndModal(1);
                    }
                }
                break;
            case 2:
                EndModal(0);
                break;
            }
        }
        if (mask & EPBM_UP) {
            name_[currentChar_] += 1;
            lastChar_ = name_[currentChar_];
            isDirty_ = true;
        }
        if (mask & EPBM_DOWN) {
            name_[currentChar_] -= 1;
            lastChar_ = name_[currentChar_];
            isDirty_ = true;
        }
    } else {
        if (mask & EPBM_R) {
        } else {
            // No modifier
            if (mask == EPBM_UP) {
                selected_ = (selected_ == 0) ? 1 : 0;
                isDirty_ = true;
            }
            if (mask == EPBM_DOWN) {
                selected_ = (selected_ == 0) ? 1 : 0;
                isDirty_ = true;
            }
            if (mask == EPBM_LEFT) {
                switch (selected_) {
                case 0:
                    if (currentChar_ > 0)
                        currentChar_--;
                    break;
                case 1:
                case 2:
                    if (selected_ > 0)
                        selected_--;
                    break;
                }
                isDirty_ = true;
            }
            if (mask == EPBM_RIGHT) {
                switch (selected_) {
                case 0:
                    if (currentChar_ < SAVE_PRESET_MAX_NAME - 1)
                        currentChar_++;
                    else
                        selected_++;
                    break;
                case 1:
                case 2:
                    if (selected_ < SAVE_PRESET_BUTTONS)
                        selected_++;
                    break;
                }
                isDirty_ = true;
            }
        }
    }
};

std::string SavePresetDialog::GetName() {
    // Trim trailing spaces
    int end = SAVE_PRESET_MAX_NAME - 1;
    while (end >= 0 && name_[end] == ' ') {
        name_[end] = 0;
        end--;
    }
    return std::string(name_);
}
