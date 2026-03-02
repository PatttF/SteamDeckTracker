#ifndef _UI_VST3_PARAMETER_FIELD_H_
#define _UI_VST3_PARAMETER_FIELD_H_

#include "UIIntVarField.h"

class VST3Instrument;

class UIVST3ParameterField: public UIIntVarField {

public:
	UIVST3ParameterField(
		GUIPoint &position,
		Variable &v,
		const char *paramName,
		VST3Instrument *instrument,
		int paramIndex,
		int min,
		int max,
		int xOffset,
		int yOffset);
		
	virtual ~UIVST3ParameterField() {};
	virtual void Draw(GUIWindow &w, int offset = 0) override;
	int GetParamIndex() const { return paramIndex_; }
	static void SetLearnMode(bool active) { s_learnMode_ = active; }

protected:
	static bool s_learnMode_;
	VST3Instrument *instrument_;
	int paramIndex_;
	char nameBuffer_[80];
};

#endif
