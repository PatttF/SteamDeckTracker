#include "UILV2EffectParameterField.h"
#include "Application/Instruments/LV2Effect.h"
#include "System/Console/Trace.h"
#include "Application/AppWindow.h"

UILV2EffectParameterField::UILV2EffectParameterField(
	GUIPoint &position,
	Variable &v,
	const char *paramName,
	LV2Effect *effect,
	int paramIndex,
	int min,
	int max,
	int xOffset,
	int yOffset) : UIIntVarField(position, v, "%s: %s", min, max, xOffset, yOffset),
	effect_(effect),
	paramIndex_(paramIndex) {
	
	snprintf(nameBuffer_, 80, "%s", paramName);
}

void UILV2EffectParameterField::Draw(GUIWindow &w, int offset) {
	GUITextProperties props;
	GUIPoint position = GetPosition();
	position._y += offset;

	if (focus_) {
		((AppWindow&)w).SetColor(CD_HILITE2);
		props.invert_ = true;
	} else {
		((AppWindow&)w).SetColor(CD_NORMAL);
	}

	char buffer[80];
	int scaledValue = src_.GetInt();
	
	if (!effect_ || effect_->IsEmpty()) {
		snprintf(buffer, 80, "%s: %d", nameBuffer_, scaledValue);
		w.DrawString(buffer, position, props);
		return;
	}
	
	const LV2PluginParameter *param = effect_->GetParameter(paramIndex_);
	if (param) {
		float realValue = param->minValue + 
			(scaledValue / 127.0f) * (param->maxValue - param->minValue);
		
		std::string label = effect_->GetParameterScalePointLabel(paramIndex_, realValue);
		if (!label.empty()) {
			snprintf(buffer, 80, "%s: %s", nameBuffer_, label.c_str());
		} else {
			snprintf(buffer, 80, "%s: %.2f", nameBuffer_, realValue);
		}
	} else {
		snprintf(buffer, 80, "%s: %d", nameBuffer_, scaledValue);
	}

	w.DrawString(buffer, position, props);
}
