#include "UIVST3ParameterField.h"
#include "Application/Instruments/VST3Instrument.h"
#include "Application/AppWindow.h"
#include <cstring>

bool UIVST3ParameterField::s_learnMode_ = false;

UIVST3ParameterField::UIVST3ParameterField(
	GUIPoint &position,
	Variable &v,
	const char *paramName,
	VST3Instrument *instrument,
	int paramIndex,
	int min,
	int max,
	int xOffset,
	int yOffset) : UIIntVarField(position, v, "%s: %s", min, max, xOffset, yOffset),
	instrument_(instrument),
	paramIndex_(paramIndex) {
	
	snprintf(nameBuffer_, 80, "%s", paramName);
}

void UIVST3ParameterField::Draw(GUIWindow &w, int offset) {
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

	if (!instrument_ || instrument_->IsEmpty()) {
		snprintf(buffer, 80, "%s: ?", nameBuffer_);
		w.DrawString(buffer, position, props);
		return;
	}

	const VST3PluginParameter *param = instrument_->GetParameter(paramIndex_);
	if (!param) {
		snprintf(buffer, 80, "%s: ?", nameBuffer_);
		w.DrawString(buffer, position, props);
		return;
	}

	// Use the instrument's current normalized value (kept in sync by CC and UI edits)
	double normalized = param->currentValue;
	if (normalized < 0.0) normalized = 0.0;
	if (normalized > 1.0) normalized = 1.0;

	// Ask the plugin for a display string (e.g. "Saw", "Square", "-6.0 dB")
	std::string displayStr = instrument_->GetParameterDisplayString(paramIndex_, normalized);
	if (!displayStr.empty()) {
		snprintf(buffer, 80, "%s: %s", nameBuffer_, displayStr.c_str());
	} else {
		// Fallback: show plain value
		double plain = param->minValue + normalized * (param->maxValue - param->minValue);
		snprintf(buffer, 80, "%s: %.2f", nameBuffer_, plain);
	}

	// Append user-learned CC binding indicator (e.g. "~74") only in learn mode
	if (s_learnMode_) {
		int ucc = instrument_->GetUserCCForParam(paramIndex_);
		if (ucc >= 0) {
			char ccTag[8]; snprintf(ccTag, sizeof(ccTag), "~%d", ucc);
			if (strlen(buffer) + strlen(ccTag) < 79) strcat(buffer, ccTag);
		}
	}

	w.DrawString(buffer, position, props);
}
