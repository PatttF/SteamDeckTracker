#ifndef _UI_LEARNABLE_INT_VAR_FIELD_H_
#define _UI_LEARNABLE_INT_VAR_FIELD_H_

#include "UIIntVarField.h"

// A UIIntVarField subclass that, while MIDI learn mode is active,
// appends "~CC" to the label if a user CC binding exists for this Variable.
// Used for Sample and SF2 instrument parameters.
// The CC annotation is pushed from InstrumentView via SetLearnCC() each
// time the view redraws, avoiding any direct instrument pointer access here.
class UILearnableIntVarField : public UIIntVarField {
public:
    UILearnableIntVarField(
        GUIPoint &position,
        Variable &v,
        const char *format,
        int min,
        int max,
        int xOffset,
        int yOffset,
        int displayOffset = 0);

    virtual ~UILearnableIntVarField() {}
    virtual void Draw(GUIWindow &w, int offset = 0) override;

    int GetMin()  const { return min_; }
    int GetMax()  const { return max_; }

    // Called by InstrumentView before redraw to set the cached CC assignment (-1 = none).
    void SetLearnCC(int cc) { learnCC_ = cc; }
    int  GetLearnCC()  const { return learnCC_; }

    static void SetLearnMode(bool active) { s_learnMode_ = active; }
    static bool s_learnMode_;

private:
    int learnCC_;   // -1 = no CC bound; pushed by InstrumentView each frame in learn mode
};

#endif
