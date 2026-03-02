#include "UILearnableIntVarField.h"
#include "Application/AppWindow.h"
#include "Foundation/Variables/Variable.h"
#include <cstring>
#include <cstdio>

bool UILearnableIntVarField::s_learnMode_ = false;

UILearnableIntVarField::UILearnableIntVarField(
    GUIPoint &position,
    Variable &v,
    const char *format,
    int min,
    int max,
    int xOffset,
    int yOffset,
    int displayOffset)
    : UIIntVarField(position, v, format, min, max, xOffset, yOffset, displayOffset),
      learnCC_(-1) {}

void UILearnableIntVarField::Draw(GUIWindow &w, int offset) {
    GUITextProperties props;
    GUIPoint position = GetPosition();
    position._y += offset;

    if (focus_) {
        ((AppWindow &)w).SetColor(CD_HILITE2);
        props.invert_ = true;
    } else {
        ((AppWindow &)w).SetColor(CD_NORMAL);
    }

    char buffer[80];
    Variable::Type type = src_.GetType();
    switch (type) {
        case Variable::INT: {
            int v = src_.GetInt() + displayOffset_;
            snprintf(buffer, sizeof(buffer), format_, v, v);
            break;
        }
        case Variable::CHAR_LIST:
        case Variable::BOOL:
            snprintf(buffer, sizeof(buffer), format_, src_.GetString());
            break;
        default:
            strcpy(buffer, "++wtf++");
            break;
    }

    if (s_learnMode_ && learnCC_ >= 0) {
        char ccTag[8];
        snprintf(ccTag, sizeof(ccTag), "~%d", learnCC_);
        if (strlen(buffer) + strlen(ccTag) < sizeof(buffer) - 1)
            strcat(buffer, ccTag);
    }

    w.DrawString(buffer, position, props);
}
