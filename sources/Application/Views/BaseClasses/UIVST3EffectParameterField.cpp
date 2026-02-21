#include "UIVST3EffectParameterField.h"
#include "Application/Instruments/VST3Effect.h"
#include "Application/AppWindow.h"

UIVST3EffectParameterField::UIVST3EffectParameterField(
	GUIPoint &position,
	Variable &v,
	const char *paramName,
	VST3Effect *effect,
	int paramIndex,
	int min,
	int max,
	int xOffset,
	int yOffset) : UIIntVarField(position, v, "%s: %s", min, max, xOffset, yOffset),
	effect_(effect),
	paramIndex_(paramIndex) {
	
	snprintf(nameBuffer_, 80, "%s", paramName);
}

void UIVST3EffectParameterField::Draw(GUIWindow &w, int offset) {
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
	
	const VST3PluginParameter *param = effect_->GetVST3Parameter(paramIndex_);
	if (!param) {
		snprintf(buffer, 80, "%s: %d", nameBuffer_, scaledValue);
		w.DrawString(buffer, position, props);
		return;
	}

	// Map scaled value back to normalized 0..1
	int maxVal = (param->stepCount > 0 && param->stepCount <= 255) ? param->stepCount : 127;
	double normalized = (double)scaledValue / (double)maxVal;
	if (normalized < 0.0) normalized = 0.0;
	if (normalized > 1.0) normalized = 1.0;

	// Ask the plugin for a display string (e.g. "12.0 dB", "On/Off")
	std::string displayStr = effect_->GetParameterScalePointLabel(paramIndex_, 
		(float)(param->minValue + normalized * (param->maxValue - param->minValue)));
	if (!displayStr.empty()) {
		snprintf(buffer, 80, "%s: %s", nameBuffer_, displayStr.c_str());
	} else {
		// Fallback: show plain value
		double plain = param->minValue + normalized * (param->maxValue - param->minValue);
		snprintf(buffer, 80, "%s: %.2f", nameBuffer_, plain);
	}

	w.DrawString(buffer, position, props);
}
