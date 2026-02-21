#ifndef _UI_LV2_EFFECT_PARAMETER_FIELD_H_
#define _UI_LV2_EFFECT_PARAMETER_FIELD_H_

#include "UIIntVarField.h"

class LV2Effect;

class UILV2EffectParameterField: public UIIntVarField {

public:
	UILV2EffectParameterField(
		GUIPoint &position,
		Variable &v,
		const char *paramName,
		LV2Effect *effect,
		int paramIndex,
		int min,
		int max,
		int xOffset,
		int yOffset);
		
	virtual ~UILV2EffectParameterField() {};
	virtual void Draw(GUIWindow &w, int offset = 0) override;
	
protected:
	LV2Effect *effect_;
	int paramIndex_;
	char nameBuffer_[80];
};

#endif
