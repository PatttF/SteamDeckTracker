#ifndef _INSTRUMENT_VIEW_H_
#define _INSTRUMENT_VIEW_H_

#include "Application/FX/FxPrinter.h"
#include "BaseClasses/FieldView.h"
#include "Foundation/Observable.h"
#include "ViewData.h"

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
	InstrumentType getInstrumentType() ;
	void Update(Observable &o,I_ObservableData *d) ;

private:
	Project *project_ ;
	FourCC lastFocusID_ ;
	I_Instrument *current_ ;
	int lv2ScrollOffset_ ;  // For scrolling LV2 parameters
	char lv2PluginLabel_[80];  // Persistent storage for plugin label
	char lv2ParamText_[10][80];  // Storage for up to 10 parameter labels
} ;
#endif
