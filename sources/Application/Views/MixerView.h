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
	bool invertBatt_ ;
	unsigned short lastMask_ ;
	unsigned int lastUpRepeatTime_ ;
	unsigned int lastDownRepeatTime_ ;
	unsigned int lastDrawTick_ ;
	unsigned int lastUpdateLogTick_ ;
	unsigned int lastEventPushTick_ ;
} ;
#endif
