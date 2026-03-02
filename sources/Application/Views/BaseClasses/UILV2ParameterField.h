#ifndef _UI_LV2_PARAMETER_FIELD_H_
#define _UI_LV2_PARAMETER_FIELD_H_

#include "UIIntVarField.h"

class LV2Instrument;

class UILV2ParameterField: public UIIntVarField {

public:
	UILV2ParameterField(
		GUIPoint &position,
		Variable &v,
		const char *paramName,
		LV2Instrument *instrument,
		int paramIndex,
		int min,
		int max,
		int xOffset,
		int yOffset);
		
	virtual ~UILV2ParameterField() {};
	virtual void Draw(GUIWindow &w, int offset = 0) override;
	int GetParamIndex() const { return paramIndex_; }
	static void SetLearnMode(bool active) { s_learnMode_ = active; }

protected:
	static bool s_learnMode_;
	LV2Instrument *instrument_;
	int paramIndex_;
	char nameBuffer_[80]; // Storage for parameter name formatting
};

#endif