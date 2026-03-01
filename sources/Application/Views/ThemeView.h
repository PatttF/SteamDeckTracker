#ifndef _THEME_VIEW_H_
#define _THEME_VIEW_H_

#include "BaseClasses/FieldView.h"
#include "Foundation/Observable.h"
#include "ViewData.h"
#include <string>

// Number of theme colors editable
#define THEME_COLOR_COUNT 17
// RGB components per color
#define THEME_COMPONENT_COUNT 3

struct ThemeColorEntry {
    const char *label;       // Display name
    const char *configKey;   // Config.xml key
    int colorDefIndex;       // Index into the static GUIColor array (see ThemeView.cpp)
};

class ThemeView : public FieldView, public I_Observer {
public:
    ThemeView(GUIWindow &w, ViewData *data);
    virtual ~ThemeView();

    virtual void ProcessButtonMask(unsigned short mask, bool pressed);
    virtual void DrawView();
    virtual void DrawGraphics();
    virtual void OnPlayerUpdate(PlayerEventType, unsigned int) {}
    virtual void OnFocus();

    void Update(Observable &, I_ObservableData *);

    void SaveConfigToFile();
    void SaveThemeToFile(const std::string &name);
    void LoadThemeFromFile(const std::string &path);
    void LoadDefaultColors();
    void OnThemeLoaded(const std::string &path);
    void OnThemeSaved(const std::string &name);

private:
    void updateLiveColor(int colorIndex);
    void refreshVarsFromLive();

    // Component variables for R, G, B of each color
    Variable *componentVars_[THEME_COLOR_COUNT][THEME_COMPONENT_COUNT];
    bool dirty_;
};

#endif
