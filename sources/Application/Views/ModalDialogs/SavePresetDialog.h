#ifndef _SAVE_PRESET_DIALOG_H_
#define _SAVE_PRESET_DIALOG_H_

#include "Application/Utils/KeyboardLayout.h"
#include "Application/Views/BaseClasses/ModalView.h"
#include <string>

#define SAVE_PRESET_MAX_NAME 20
#define SAVE_PRESET_BUTTONS 2

class SavePresetDialog : public ModalView {
public:
  SavePresetDialog(View &view);
  virtual ~SavePresetDialog();

  virtual void DrawView();
  virtual void OnPlayerUpdate(PlayerEventType, unsigned int currentTick);
  virtual void OnFocus();
  virtual void ProcessButtonMask(unsigned short mask, bool pressed);

  std::string GetName();

private:
  int selected_;
  int lastChar_;
  char name_[SAVE_PRESET_MAX_NAME + 1];
  int currentChar_;
  bool keyboardMode_;
  int keyboardRow_;
  int keyboardCol_;
  void moveCursor(int direction);
};
#endif
