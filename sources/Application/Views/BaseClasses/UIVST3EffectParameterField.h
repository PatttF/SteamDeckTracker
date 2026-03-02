#ifndef _UI_VST3_EFFECT_PARAMETER_FIELD_H_
#define _UI_VST3_EFFECT_PARAMETER_FIELD_H_

#include "UIIntVarField.h"

class VST3Effect;

class UIVST3EffectParameterField: public UIIntVarField {

public:
	UIVST3EffectParameterField(
		GUIPoint &position,
		Variable &v,
		const char *paramName,
		VST3Effect *effect,
		int paramIndex,
		int min,
		int max,
		int xOffset,
		int yOffset);
		
	virtual ~UIVST3EffectParameterField() {};
	virtual void Draw(GUIWindow &w, int offset = 0) override;

	int GetParamIndex() const { return paramIndex_; }
	static void SetLearnMode(bool active) { s_learnMode_ = active; }
	static bool s_learnMode_;

protected:
	VST3Effect *effect_;
	int paramIndex_;
	char nameBuffer_[80];
};

#endif
