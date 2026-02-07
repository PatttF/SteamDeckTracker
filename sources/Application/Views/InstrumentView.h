#ifndef _INSTRUMENT_VIEW_H_
#define _INSTRUMENT_VIEW_H_

#include "Application/FX/FxPrinter.h"
#include "BaseClasses/FieldView.h"
#include "Foundation/Observable.h"
#include "ViewData.h"

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
	InstrumentType getInstrumentType() ;
	void Update(Observable &o,I_ObservableData *d) ;

public:
	void OnLV2PluginSelected() ;  // Callback when LV2 plugin is selected
	void OnSF2Selected() ;        // Callback when SF2 file/preset is selected

private:
	Project *project_ ;
	FourCC lastFocusID_ ;
	I_Instrument *current_ ;
	int lv2ScrollOffset_ ;  // For scrolling LV2 parameters
	char lv2PluginLabel_[80];  // Persistent storage for plugin label
	char lv2ParamText_[40][40];  // Storage for parameter labels (2 cols x 20 rows)
	// Action field for loading LV2 list (clickable)
	UIActionField *lv2LoadField_ ;
	// Action field for loading SF2 browser (clickable)
	UIActionField *sf2LoadField_ ;
	char sf2Label_[80];  // Storage for SF2 display label
	char sf2PresetLabel_[80];  // Storage for SF2 preset display label
	// Track last selected 'type' per instrument so we can detect changes and switch types
	int lastType_[MAX_INSTRUMENT_COUNT];

	// Deferred type switch (set by Update, executed in ProcessButtonMask to avoid deleting
	// the instrument while it's still being notified)
	int pendingTypeInstrumentIdx_;
	InstrumentType pendingType_;
} ;
#endif
