#ifndef _INSTRUMENT_VIEW_H_
#define _INSTRUMENT_VIEW_H_

#include "Application/FX/FxPrinter.h"
#include "BaseClasses/FieldView.h"
#include "Foundation/Observable.h"
#include "ViewData.h"
#include <string>

class UIActionField; // forward declaration to avoid header dependency

class InstrumentView: public FieldView, public I_Observer {
public:
	InstrumentView(GUIWindow &w,ViewData *data) ;
	virtual ~InstrumentView() ;

	virtual void ProcessButtonMask(unsigned short mask,bool pressed) ;
	virtual void DrawView() ;
	virtual void OnPlayerUpdate(PlayerEventType,unsigned int) {} ;
	virtual void OnFocus() ;

protected:
	void warpToNext(int offset) ;
	void onInstrumentChange() ;
	void fillSampleParameters() ;
	void fillMidiParameters() ;
	void fillLV2Parameters() ;
	void fillSoundFontParameters() ;
	void fillVST3Parameters() ;
	InstrumentType getInstrumentType() ;
	void Update(Observable &o,I_ObservableData *d) ;

public:
	void OnLV2PluginSelected() ;  // Callback when LV2 plugin is selected
	void OnSF2Selected() ;        // Callback when SF2 file/preset is selected
	void OnVST3PluginSelected() ; // Callback when VST3 plugin is selected
	void OnSavePreset(const std::string &name) ;  // Callback when save preset name is entered

private:
	Project *project_ ;
	FourCC lastFocusID_ ;
	I_Instrument *current_ ;
	int lv2ScrollOffset_ ;  // For scrolling LV2 parameters
	char lv2PluginLabel_[80];  // Persistent storage for plugin label
	char lv2ParamText_[40][40];  // Storage for parameter labels (2 cols x 20 rows)
	char lv2BankLabel_[80];  // Storage for LV2 bank name display
	char lv2PresetLabel_[80];  // Storage for LV2 preset name display
	// Action field for loading LV2 list (clickable)
	UIActionField *lv2LoadField_ ;
	// Action field for saving LV2 preset (clickable)
	UIActionField *lv2SaveField_ ;
	// Action field for loading SF2 browser (clickable)
	UIActionField *sf2LoadField_ ;
	// Action field for loading VST3 browser (clickable)
	UIActionField *vst3LoadField_ ;
	// Action field for saving VST3 preset (clickable)
	UIActionField *vst3SaveField_ ;
	char sf2Label_[80];  // Storage for SF2 display label
	char sf2PresetLabel_[80];  // Storage for SF2 preset display label
	char vst3PluginLabel_[80]; // Storage for VST3 plugin label
	char vst3ParamText_[40][40]; // Storage for VST3 parameter labels
	char vst3PresetLabel_[80]; // Storage for VST3 preset name display
	char vst3BankLabel_[80]; // Storage for VST3 bank name display
	int vst3ScrollOffset_ ;  // For scrolling VST3 parameters
	// Track last selected 'type' per instrument so we can detect changes and switch types
	int lastType_[MAX_INSTRUMENT_COUNT];

	// Deferred type switch (set by Update, executed in ProcessButtonMask to avoid deleting
	// the instrument while it's still being notified)
	int pendingTypeInstrumentIdx_;
	InstrumentType pendingType_;
} ;
#endif
