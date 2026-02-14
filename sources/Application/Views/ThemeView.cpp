#include "ThemeView.h"
#include "Application/AppWindow.h"
#include "Application/Model/Config.h"
#include "Application/Utils/char.h"
#include "BaseClasses/UIActionField.h"
#include "BaseClasses/UIIntVarField.h"
#include "BaseClasses/UIStaticField.h"
#include "System/Console/Trace.h"
#include "Externals/TinyXML/tinyxml.h"
#include "System/FileSystem/FileSystem.h"
#include "Adapters/SDL2/GUI/SDLGUIWindowImp.h"
#include "ModalDialogs/SavePresetDialog.h"
#include "ModalDialogs/LoadThemeDialog.h"
#include <cstring>
#include <cstdio>

#define ACTION_THEME_APPLY MAKE_FOURCC('T','A','P','L')
#define ACTION_THEME_SAVE  MAKE_FOURCC('T','S','A','V')
#define ACTION_THEME_LOAD  MAKE_FOURCC('T','L','O','D')
#define ACTION_THEME_DEFAULT MAKE_FOURCC('T','D','E','F')

// Color table: label, config key, and pointer offset handled in updateLiveColor
static const ThemeColorEntry themeColors[THEME_COLOR_COUNT] = {
    {"Background",  "BACKGROUND",   0},
    {"Foreground",  "FOREGROUND",   1},
    {"Border",      "BORDER",       2},
    {"Clip",        "CLIPCOLOR",    3},
    {"SongView FE", "SONGVIEW_FE",  4},
    {"SongView 00", "SONGVIEW_00",  5},
    {"Highlight1",  "HICOLOR1",     6},
    {"Highlight2",  "HICOLOR2",     7},
    {"Console",     "CONSOLECOLOR", 8},
    {"Cursor",      "CURSORCOLOR",  9},
    {"Play",        "PLAYCOLOR",    10},
    {"Mute",        "MUTECOLOR",    11},
    {"Row",         "ROWCOLOR1",    12},
    {"Row2/Beat",   "ROWCOLOR2",    13},
};

// Access the static GUIColor members via pointer array
// Order must match colorDefIndex above
static GUIColor *getColorPtr(int index) {
    // These are the static members declared in AppWindow
    // We access them by extern reference since they are public static
    switch (index) {
    case 0:  return &AppWindow::backgroundColor_;
    case 1:  return &AppWindow::normalColor_;
    case 2:  return &AppWindow::borderColor_;
    case 3:  return &AppWindow::clipColor_;
    case 4:  return &AppWindow::songviewfeColor_;
    case 5:  return &AppWindow::songview00Color_;
    case 6:  return &AppWindow::highlightColor_;
    case 7:  return &AppWindow::highlight2Color_;
    case 8:  return &AppWindow::consoleColor_;
    case 9:  return &AppWindow::cursorColor_;
    case 10: return &AppWindow::playColor_;
    case 11: return &AppWindow::muteColor_;
    case 12: return &AppWindow::rownumberColor_;
    case 13: return &AppWindow::rownumber2Color_;
    default: return nullptr;
    }
}

// Default colors matching the hardcoded values in AppWindow.cpp
static const unsigned char defaultColors[THEME_COLOR_COUNT][3] = {
    {0x1D, 0x0A, 0x1F},  // Background
    {0xF5, 0xEB, 0xFF},  // Foreground
    {0xFF, 0x00, 0x8C},  // Border
    {0xFF, 0x00, 0x00},  // Clip
    {0xA5, 0x5B, 0x8F},  // SongView FE
    {0x85, 0x3B, 0x6F},  // SongView 00
    {0xB7, 0x50, 0xD1},  // Highlight1
    {0xDB, 0x33, 0xDB},  // Highlight2
    {0x00, 0xFF, 0x00},  // Console
    {0xFF, 0x00, 0x8C},  // Cursor
    {0xFF, 0x00, 0x8C},  // Play
    {0xF5, 0xEB, 0xFF},  // Mute
    {0xBA, 0x28, 0xF9},  // Row
    {0xFF, 0x00, 0xFF},  // Row2/Beat
};

// Callbacks for modal dialogs
static void SaveThemeCallback(View &v, ModalView &dialog) {
    SavePresetDialog &spd = (SavePresetDialog &)dialog;
    if (dialog.GetReturnCode() > 0) {
        std::string name = spd.GetName();
        if (!name.empty()) {
            ThemeView &tv = (ThemeView &)v;
            tv.OnThemeSaved(name);
        }
    }
}

static void LoadThemeCallback(View &v, ModalView &dialog) {
    LoadThemeDialog &ltd = (LoadThemeDialog &)dialog;
    if (dialog.GetReturnCode() > 0) {
        std::string path = ltd.GetSelection();
        if (!path.empty()) {
            ThemeView &tv = (ThemeView &)v;
            tv.OnThemeLoaded(path);
        }
    }
}

ThemeView::ThemeView(GUIWindow &w, ViewData *data) : FieldView(w, data) {
    dirty_ = false;

    GUIPoint position = GetAnchor();
    position._y += 1; // Leave room for title

    // Column header: R  G  B
    // Label is at x=0, R at x+13, G at x+16, B at x+19

    for (int i = 0; i < THEME_COLOR_COUNT; i++) {
        // Get current color value from the live GUIColor
        GUIColor *gc = getColorPtr(i);
        unsigned char r = gc ? (gc->_r & 0xFF) : 0;
        unsigned char g = gc ? (gc->_g & 0xFF) : 0;
        unsigned char b = gc ? (gc->_b & 0xFF) : 0;

        // Create label (static field, not navigable but takes a slot)
        UIStaticField *label = new UIStaticField(position, themeColors[i].label);
        T_SimpleList<UIField>::Insert(label);

        // Create R, G, B component variables and fields
        unsigned char rgb[3] = {r, g, b};
        for (int c = 0; c < THEME_COMPONENT_COUNT; c++) {
            // FourCC encodes color index and component: upper byte = color, lower = component
            FourCC id = MAKE_FOURCC('C', (char)('0' + (i / 10)), (char)('0' + (i % 10)), (char)('0' + c));
            componentVars_[i][c] = new Variable(themeColors[i].label, id, (int)rgb[c]);

            GUIPoint fieldPos = position;
            fieldPos._x += 13 + c * 3;

            UIIntVarField *field = new UIIntVarField(
                fieldPos, *componentVars_[i][c], "%2.2X", 0, 255, 1, 16);
            T_SimpleList<UIField>::Insert(field);
        }

        position._y += 1;

        // After 7 colors, check if we need more room (screen is 30 rows)
        // We have title at y=0, header at anchor+1, so 14 colors fit fine
    }

    // Add buttons at the bottom
    position._y += 1;
    UIActionField *applyBtn = new UIActionField("Apply", ACTION_THEME_APPLY, position);
    applyBtn->AddObserver(*this);
    T_SimpleList<UIField>::Insert(applyBtn);

    position._x += 7;
    UIActionField *saveBtn = new UIActionField("Save", ACTION_THEME_SAVE, position);
    saveBtn->AddObserver(*this);
    T_SimpleList<UIField>::Insert(saveBtn);

    position._x += 6;
    UIActionField *loadBtn = new UIActionField("Load", ACTION_THEME_LOAD, position);
    loadBtn->AddObserver(*this);
    T_SimpleList<UIField>::Insert(loadBtn);

    position._x += 6;
    UIActionField *defaultBtn = new UIActionField("Load Default", ACTION_THEME_DEFAULT, position);
    defaultBtn->AddObserver(*this);
    T_SimpleList<UIField>::Insert(defaultBtn);

}

ThemeView::~ThemeView() {
    for (int i = 0; i < THEME_COLOR_COUNT; i++) {
        for (int c = 0; c < THEME_COMPONENT_COUNT; c++) {
            delete componentVars_[i][c];
        }
    }
}

void ThemeView::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed)
        return;

    // B alone: exit back to project view
    if (mask == EPBM_B) {
        ViewType vt = VT_PROJECT;
        ViewEvent ve(VET_SWITCH_VIEW, &vt);
        SetChanged();
        NotifyObservers(&ve);
        return;
    }

    FieldView::ProcessButtonMask(mask);

    // After any input, sync all live colors from current variable values
    for (int i = 0; i < THEME_COLOR_COUNT; i++) {
        GUIColor *gc = getColorPtr(i);
        if (!gc) continue;
        unsigned char r = (unsigned char)(componentVars_[i][0]->GetInt() & 0xFF);
        unsigned char g = (unsigned char)(componentVars_[i][1]->GetInt() & 0xFF);
        unsigned char b = (unsigned char)(componentVars_[i][2]->GetInt() & 0xFF);
        if (gc->_r != r || gc->_g != g || gc->_b != b) {
            gc->_r = r;
            gc->_g = g;
            gc->_b = b;
            dirty_ = true;

            // Update config variable too
            Config *config = Config::GetInstance();
            char hexStr[7];
            sprintf(hexStr, "%02X%02X%02X", r, g, b);
            Variable *v = config->FindVariable(themeColors[i].configKey);
            if (v) {
                v->SetString(hexStr, false);
            } else {
                Variable *nv = new Variable(themeColors[i].configKey, 0, hexStr);
                config->Insert(nv);
            }
        }
    }

    if (mask & EPBM_START) {
        Player *player = Player::GetInstance();
        player->OnStartButton(PM_SONG, viewData_->songX_, false,
                              viewData_->songX_);
    }

    isDirty_ = true;
}

void ThemeView::DrawView() {
    Clear();

    // Always clear swatch pixel areas with background color BEFORE modal renders.
    // This runs before the modal's Redraw(), so modal content will draw on top correctly.
    {
        SDLGUIWindowImp *imp = (SDLGUIWindowImp *)w_.GetImpWindow();
        if (imp) {
            GUIPoint anchor = GetAnchor();
            const int cw = 8;
            const int ch = 8;
            GUIColor bg(AppWindow::backgroundColor_._r,
                        AppWindow::backgroundColor_._g,
                        AppWindow::backgroundColor_._b);
            imp->SetColor(bg);
            for (int i = 0; i < THEME_COLOR_COUNT; i++) {
                int swatchX = (anchor._x + 11) * cw;
                int swatchY = (anchor._y + 1 + i) * ch;
                GUIRect r(swatchX, swatchY, swatchX + 2 * cw, swatchY + ch);
                imp->DrawRect(r);
            }
        }
    }

    GUITextProperties props;
    GUIPoint pos = GetTitlePosition();

    SetColor(CD_NORMAL);
    DrawString(pos._x, pos._y, "Theme", props);

    // Column headers
    GUIPoint anchor = GetAnchor();
    DrawString(anchor._x + 13, anchor._y, " R  G  B", props);

    // Exit hint at bottom
    DrawString(53, 28, "Press B to exit", props);

    FieldView::Redraw();
}

void ThemeView::DrawGraphics() {
    // Don't draw swatches when a modal dialog is open — the swatch pixel areas
    // were already cleared to background in DrawView() before the modal rendered.
    if (HasModal()) return;

    SDLGUIWindowImp *imp = (SDLGUIWindowImp *)w_.GetImpWindow();
    if (!imp) return;

    SDL_Renderer *renderer = imp->GetRenderer();
    if (!renderer) return;
    int mult = imp->GetMult();
    int ax = imp->GetAppAnchorX();
    int ay = imp->GetAppAnchorY();

    GUIPoint anchor = GetAnchor();
    const int cw = 8;
    const int ch = 8;

    for (int i = 0; i < THEME_COLOR_COUNT; i++) {
        GUIColor *gc = getColorPtr(i);
        if (!gc) continue;

        int swatchX = (anchor._x + 11) * cw;
        int swatchY = (anchor._y + 1 + i) * ch;
        int swatchW = 2 * cw;
        int swatchH = ch;

        SDL_SetRenderDrawColor(renderer, gc->_r & 0xFF, gc->_g & 0xFF, gc->_b & 0xFF, 0xFF);
        SDL_Rect swatchRect = { swatchX * mult + ax, swatchY * mult + ay, swatchW * mult, swatchH * mult };
        SDL_RenderFillRect(renderer, &swatchRect);
    }
}

void ThemeView::OnFocus() {
    // Refresh component vars from current live colors
    for (int i = 0; i < THEME_COLOR_COUNT; i++) {
        GUIColor *gc = getColorPtr(i);
        if (gc) {
            componentVars_[i][0]->SetInt(gc->_r & 0xFF, false);
            componentVars_[i][1]->SetInt(gc->_g & 0xFF, false);
            componentVars_[i][2]->SetInt(gc->_b & 0xFF, false);
        }
    }
}

void ThemeView::updateLiveColor(int colorIndex) {
    if (colorIndex < 0 || colorIndex >= THEME_COLOR_COUNT) return;

    unsigned char r = (unsigned char)(componentVars_[colorIndex][0]->GetInt() & 0xFF);
    unsigned char g = (unsigned char)(componentVars_[colorIndex][1]->GetInt() & 0xFF);
    unsigned char b = (unsigned char)(componentVars_[colorIndex][2]->GetInt() & 0xFF);

    GUIColor *gc = getColorPtr(colorIndex);
    if (gc) {
        gc->_r = r;
        gc->_g = g;
        gc->_b = b;
    }

    // Also update the Config variable so GetValue() returns the new value
    Config *config = Config::GetInstance();
    char hexStr[7];
    sprintf(hexStr, "%02X%02X%02X", r, g, b);
    Variable *v = config->FindVariable(themeColors[colorIndex].configKey);
    if (v) {
        v->SetString(hexStr, false);
    } else {
        Variable *nv = new Variable(themeColors[colorIndex].configKey, 0, hexStr);
        config->Insert(nv);
    }
}

void ThemeView::Update(Observable &o, I_ObservableData *d) {
    if (!hasFocus_)
        return;

    UIField *focus = GetFocus();
    focus->ClearFocus();
    focus->Draw(w_);
    w_.Flush();
    focus->SetFocus();

#ifdef _64BIT
    int fourcc = *((unsigned int *)d);
#else
    int fourcc = (int)(intptr_t)d;
#endif

    if (fourcc == ACTION_THEME_APPLY) {
        // Sync all current values to config and save config.xml
        for (int i = 0; i < THEME_COLOR_COUNT; i++) {
            updateLiveColor(i);
        }
        SaveConfigToFile();
        dirty_ = false;
        // Navigate back to project view
        ViewType vt = VT_PROJECT;
        ViewEvent ve(VET_SWITCH_VIEW, &vt);
        SetChanged();
        NotifyObservers((I_ObservableData *)&ve);
    } else if (fourcc == ACTION_THEME_SAVE) {
        // Open save dialog to name the theme file
        SavePresetDialog *dialog = new SavePresetDialog(*this);
        DoModal(dialog, SaveThemeCallback);
    } else if (fourcc == ACTION_THEME_LOAD) {
        // Open load dialog to pick a theme file
        LoadThemeDialog *dialog = new LoadThemeDialog(*this);
        DoModal(dialog, LoadThemeCallback);
    } else if (fourcc == ACTION_THEME_DEFAULT) {
        LoadDefaultColors();
        SetNotification("Defaults loaded");
    }

    focus->Draw(w_);
    isDirty_ = true;
}

void ThemeView::SaveConfigToFile() {
    // Build an XML document from current config values and write to root:config.xml
    Path path("root:config.xml");
    std::string configPath = path.GetPath();

    TiXmlDocument doc;
    TiXmlDeclaration *decl = new TiXmlDeclaration("1.0", "", "");
    doc.LinkEndChild(decl);

    TiXmlElement *root = new TiXmlElement("CONFIG");
    doc.LinkEndChild(root);

    // Write all current Config variables
    Config *config = Config::GetInstance();

    // List of all known config keys to persist
    const char *keys[] = {
        "DUMPEVENT", "SCREENMULT",
        "BACKGROUND", "FOREGROUND", "BORDER", "CLIPCOLOR",
        "SONGVIEW_FE", "SONGVIEW_00",
        "HICOLOR1", "HICOLOR2", "CONSOLECOLOR",
        "CURSORCOLOR", "PLAYCOLOR", "MUTECOLOR",
        "ROWCOLOR1", "ROWCOLOR2", "MAJORBEAT",
        "ALTROWNUMBER", "FONTTYPE",
        nullptr
    };

    for (int i = 0; keys[i] != nullptr; i++) {
        const char *val = config->GetValue(keys[i]);
        if (val) {
            TiXmlElement *elem = new TiXmlElement(keys[i]);
            elem->SetAttribute("value", val);
            root->LinkEndChild(elem);
        }
    }

    if (doc.SaveFile(configPath.c_str())) {
        Trace::Log("THEME", "Saved config to %s", configPath.c_str());
    } else {
        Trace::Log("THEME", "Failed to save config to %s", configPath.c_str());
    }
}

void ThemeView::refreshVarsFromLive() {
    for (int i = 0; i < THEME_COLOR_COUNT; i++) {
        GUIColor *gc = getColorPtr(i);
        if (gc) {
            componentVars_[i][0]->SetInt(gc->_r & 0xFF, false);
            componentVars_[i][1]->SetInt(gc->_g & 0xFF, false);
            componentVars_[i][2]->SetInt(gc->_b & 0xFF, false);
        }
    }
    isDirty_ = true;
}

void ThemeView::SaveThemeToFile(const std::string &name) {
    // Save current color values to root:themes/name.theme
    Path themesDir("root:themes");
    std::string filePath = themesDir.GetPath() + "/" + name + ".theme";

    TiXmlDocument doc;
    TiXmlDeclaration *decl = new TiXmlDeclaration("1.0", "", "");
    doc.LinkEndChild(decl);

    TiXmlElement *root = new TiXmlElement("THEME");
    doc.LinkEndChild(root);

    for (int i = 0; i < THEME_COLOR_COUNT; i++) {
        GUIColor *gc = getColorPtr(i);
        if (!gc) continue;
        char hexStr[7];
        sprintf(hexStr, "%02X%02X%02X", gc->_r & 0xFF, gc->_g & 0xFF, gc->_b & 0xFF);
        TiXmlElement *elem = new TiXmlElement(themeColors[i].configKey);
        elem->SetAttribute("value", hexStr);
        root->LinkEndChild(elem);
    }

    if (doc.SaveFile(filePath.c_str())) {
        Trace::Log("THEME", "Saved theme to %s", filePath.c_str());
        char msg[64];
        snprintf(msg, sizeof(msg), "Saved: %s", name.c_str());
        SetNotification(msg);
    } else {
        Trace::Log("THEME", "Failed to save theme to %s", filePath.c_str());
        SetNotification("Save failed!");
    }
}

void ThemeView::LoadThemeFromFile(const std::string &path) {
    TiXmlDocument doc(path.c_str());
    if (!doc.LoadFile()) {
        Trace::Log("THEME", "Failed to load theme from %s", path.c_str());
        SetNotification("Load failed!");
        return;
    }

    TiXmlElement *root = doc.RootElement();
    if (!root) return;

    for (int i = 0; i < THEME_COLOR_COUNT; i++) {
        TiXmlElement *elem = root->FirstChildElement(themeColors[i].configKey);
        if (elem) {
            const char *val = elem->Attribute("value");
            if (val && strlen(val) >= 6) {
                unsigned char r, g, b;
                char2hex(val, &r);
                char2hex(val + 2, &g);
                char2hex(val + 4, &b);

                GUIColor *gc = getColorPtr(i);
                if (gc) {
                    gc->_r = r;
                    gc->_g = g;
                    gc->_b = b;
                }

                // Update config too
                Config *config = Config::GetInstance();
                Variable *v = config->FindVariable(themeColors[i].configKey);
                if (v) {
                    v->SetString(val, false);
                } else {
                    Variable *nv = new Variable(themeColors[i].configKey, 0, val);
                    config->Insert(nv);
                }
            }
        }
    }

    refreshVarsFromLive();
    dirty_ = true;
    Trace::Log("THEME", "Loaded theme from %s", path.c_str());
}

void ThemeView::LoadDefaultColors() {
    for (int i = 0; i < THEME_COLOR_COUNT; i++) {
        GUIColor *gc = getColorPtr(i);
        if (gc) {
            gc->_r = defaultColors[i][0];
            gc->_g = defaultColors[i][1];
            gc->_b = defaultColors[i][2];
        }

        // Update config
        Config *config = Config::GetInstance();
        char hexStr[7];
        sprintf(hexStr, "%02X%02X%02X", defaultColors[i][0], defaultColors[i][1], defaultColors[i][2]);
        Variable *v = config->FindVariable(themeColors[i].configKey);
        if (v) {
            v->SetString(hexStr, false);
        } else {
            Variable *nv = new Variable(themeColors[i].configKey, 0, hexStr);
            config->Insert(nv);
        }
    }

    refreshVarsFromLive();
    dirty_ = true;
}

void ThemeView::OnThemeSaved(const std::string &name) {
    SaveThemeToFile(name);
}

void ThemeView::OnThemeLoaded(const std::string &path) {
    LoadThemeFromFile(path);

    // Extract name from path for notification
    std::string name = path;
    size_t slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // Remove .theme extension
    if (name.size() > 6 && name.substr(name.size() - 6) == ".theme") {
        name = name.substr(0, name.size() - 6);
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Loaded: %s", name.c_str());
    SetNotification(msg);
}
