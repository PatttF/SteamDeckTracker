#include "MessageBox.h"

static const char *buttonText[MBL_LAST] = {
	"Ok",
	"Yes",
	"Cancel",
	"No"
} ;

MessageBox::MessageBox(View &view,const char *message,int btnFlags):
	ModalView(view),
	message_(message) {

	buttonCount_=0 ;
	for (int i=0;i<MBL_LAST;i++) {
		if (btnFlags&(1<<(i))) {
			button_[buttonCount_]=i ;
			buttonCount_++ ;
		}
	}
	selected_=buttonCount_-1 ;
	NAssert(buttonCount_!=0) ;
} ;

MessageBox::~MessageBox() {
} ;

void MessageBox::DrawView() {
    // full message length
    int msgLen = (int)message_.size();

    // compute space needed for buttons
    int btnSize = 5;
    int btnArea = buttonCount_ * (btnSize + 1) + 1;

    // Determine width capped by ModalView max (36)
    int width = btnArea;
    if (msgLen > width) width = msgLen;
    if (width > 36) width = 36;

    // If message is longer than width, wrap into two lines
    bool wrapped = false;
    std::string line1, line2;
    if (msgLen > width) {
        wrapped = true;
        // Try to split at last space before width
        int split = width;
        while (split > 0 && message_[split] != ' ') split--;
        if (split <= 0) split = width; // no space found
        line1 = message_.substr(0, split);
        // Skip leading space on second line
        int start2 = split;
        if (start2 < (int)message_.size() && message_[start2] == ' ') start2++;
        line2 = message_.substr(start2);
    } else {
        line1 = message_;
    }

    // Set window height depending on wrapping
    int height = wrapped ? 4 : 3;
    SetWindow(width, height);

    // Draw message lines centered
    GUITextProperties props;
    SetColor(CD_NORMAL);
    int y = 0;
    int x = (width - (int)line1.size()) / 2;
    DrawString(x, y, line1.c_str(), props);
    if (wrapped) {
        y = 1;
        x = (width - (int)line2.size()) / 2;
        DrawString(x, y, line2.c_str(), props);
    }

    // Draw buttons on the bottom row
    y = height - 1;
    int offset = width / (buttonCount_ + 1);

    for (int i = 0; i < buttonCount_; i++) {
        const char *text = buttonText[button_[i]];
        x = offset * (i + 1) - strlen(text) / 2;
        props.invert_ = (i == selected_) ? true : false;
        DrawString(x, y, text, props);
    }
}

void MessageBox::OnPlayerUpdate(PlayerEventType ,unsigned int currentTick) {
} ;
void MessageBox::OnFocus() {
} ;
void MessageBox::ProcessButtonMask(unsigned short mask,bool pressed) {
	if (mask&EPBM_A) {
		EndModal(button_[selected_]) ;
	}
	if (mask&EPBM_LEFT) {
		selected_=(selected_+1) ;
		if (selected_>=buttonCount_) {
			selected_=0 ;
		}
	} 
	if (mask&EPBM_RIGHT) {
		selected_=(selected_-1) ;
		if (selected_<0) {
			selected_=buttonCount_-1 ;
		}
	} 
	isDirty_=true ;
} ;

