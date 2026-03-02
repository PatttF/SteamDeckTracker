#include "UILV2ParameterField.h"
#include "Application/Instruments/LV2Instrument.h"
#include "System/Console/Trace.h"
#include "Application/AppWindow.h"
#include <cstring>

bool UILV2ParameterField::s_learnMode_ = false;

UILV2ParameterField::UILV2ParameterField(
	GUIPoint &position,
	Variable &v,
	const char *paramName,
	LV2Instrument *instrument,
	int paramIndex,
	int min,
	int max,
	int xOffset,
	int yOffset) : UIIntVarField(position, v, "%s: %s", min, max, xOffset, yOffset),
	instrument_(instrument),
	paramIndex_(paramIndex) {
	
	// Store parameter name for later use
	snprintf(nameBuffer_, 80, "%s", paramName);
}

void UILV2ParameterField::Draw(GUIWindow &w, int offset) {
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
	
	// Safety check - instrument may have been changed/deleted
	if (!instrument_ || instrument_->IsEmpty()) {
		snprintf(buffer, 80, "%s: %d", nameBuffer_, scaledValue);
		w.DrawString(buffer, position, props);
		return;
	}
	
	// Try to get scale point label for this value
	if (instrument_) {
		const LV2PluginParameter *param = instrument_->GetParameter(paramIndex_);
		if (param) {
			// Use the instrument's current physical value (updated by CC or UI)
			float realValue = param->currentValue;
			
			std::string label = instrument_->GetParameterScalePointLabel(paramIndex_, realValue);
			if (!label.empty()) {
				// Use scale point label
				snprintf(buffer, 80, "%s: %s", nameBuffer_, label.c_str());
			} else {
				// Always show decimal to give visual feedback on every button press
				snprintf(buffer, 80, "%s: %.2f", nameBuffer_, realValue);
			}
		} else {
			snprintf(buffer, 80, "%s: %d", nameBuffer_, scaledValue);
		}
	} else {
		// No instrument reference, use numeric display
		snprintf(buffer, 80, "%s: %d", nameBuffer_, scaledValue);
	}

	// Append user-learned CC binding indicator (e.g. "~74") only in learn mode
	if (s_learnMode_ && instrument_) {
		int ucc = instrument_->GetUserCCForParam(paramIndex_);
		if (ucc >= 0) {
			char ccTag[8]; snprintf(ccTag, sizeof(ccTag), "~%d", ucc);
			if (strlen(buffer) + strlen(ccTag) < 79) strcat(buffer, ccTag);
		}
	}

	w.DrawString(buffer, position, props);
}