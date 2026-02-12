#ifndef _MIXER_VIEW_H_
#define _MIXER_VIEW_H_

#include "BaseClasses/View.h"
#include "ViewData.h"
#include "Foundation/Observable.h"

class MixerView: public View, public I_Observer {
public:
	MixerView(GUIWindow &w,ViewData *viewData) ;
	~MixerView() ;
	virtual void ProcessButtonMask(unsigned short mask,bool pressed) ;
	virtual void DrawView() ;
	virtual void DrawGraphics() ;
	virtual void OnPlayerUpdate(PlayerEventType ,unsigned int tick=0) ;
	virtual void OnFocus() ;
	virtual void Update(Observable &o, I_ObservableData *d) ;
protected:
	void processNormalButtonMask(unsigned int mask, bool pressed) ;
    void processSelectionButtonMask(unsigned int mask, bool pressed) ;
	void onStart() ;
	void onStop() ;
	void updateCursor(int dx,int dy)  ;
private:
	const char *song_ ;

	struct {                      // .Clipboard structure
        bool active_ ;            // .If currently making a selection
        unsigned char *data_ ;    // .Null if clipboard empty
        int x_ ;                  // .Current selection positions
        int y_ ;                  // .
        int offset_ ;             // .
        int width_ ;              // .Size of selection
        int height_ ;             // .
    } clipboard_ ;

	int saveX_ ;
	int saveY_ ;
	int saveOffset_ ;

	// Cached layout for pixel meter drawing in DrawGraphics()
	int meterLeftX_;      // char column of first meter
	int meterTopY_;       // char row of meter top
	int meterDx_;         // char column spacing per channel
	int meterGap_;        // char gap between channels
	int meterHeight_;     // meter height in char rows
	int meterChannels_;   // number of channels
	float chPeaks_[17];   // smoothed live audio peaks per channel + master
	float chPeakHold_[17]; // peak hold values
	int chPeakTimer_[17]; // peak hold decay timers
	// Oscilloscope layout cache
	int scopeLeftPx_;     // left pixel X of scope area
	int scopeTopPx_;      // top pixel Y of scope area
	int scopeWidthPx_;    // pixel width of scope area
	int scopeHeightPx_;   // pixel height of scope area

	bool invertBatt_ ;
	unsigned short lastMask_ ;
	unsigned int lastUpRepeatTime_ ;
	unsigned int lastDownRepeatTime_ ;
	unsigned int lastDrawTick_ ;
	unsigned int lastUpdateLogTick_ ;
	unsigned int lastEventPushTick_ ;
} ;
#endif
