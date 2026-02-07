#include "View.h"
#include "System/Console/Trace.h"
#include "Application/Player/Player.h"
#include "Application/Utils/char.h"
#include "Application/AppWindow.h"
#include "Application/Model/Config.h"
#include "ModalView.h"

bool View::initPrivate_=false ;

int View::margin_=0 ;
int View::songRowCount_; //=21 sets screen height among other things
bool View::miniLayout_=false ;
int View::altRowNumber_ = 4;

View::View(GUIWindow &w,ViewData *viewData):
	w_(w),
	modalView_(0),
	modalViewCallback_(0),
	hasFocus_(false),
	notificationTime_(0),
	notiDistY_(2),
	isDirty_(false),
	viewType_(VT_SONG)
{
  if (!initPrivate_) 
  {
	   GUIRect rect=w.GetRect() ;
     miniLayout_=(rect.Width()<320);
	   View::margin_=0 ;
		songRowCount_ = miniLayout_ ? 16:22; // 22 is row display count among other things

		const char *altRowStr = Config::GetInstance()->GetValue("ALTROWNUMBER");
		if (altRowStr) {
			altRowNumber_ = atoi(altRowStr);
		}

     initPrivate_=true ;
  }
	mask_=0 ;
	viewMode_=VM_NORMAL ;
	locked_=false ;
	viewData_=viewData;
	NOTIFICATION_TIMEOUT = 1000;
	displayNotification_ = "";
} ;

GUIPoint View::GetAnchor() {
	// Compute visible logical width based on actual window pixel width
	const int CHAR_PX = 8; // character pixel width
	int winPixelWidth = w_.GetRect().Width();
	int visibleCols = winPixelWidth / CHAR_PX;
	int width = (visibleCols > 0) ? std::min(visibleCols, LOGICAL_COLS) : LOGICAL_COLS;
	int height = LOGICAL_ROWS;
	return GUIPoint((width - SONG_CHANNEL_COUNT * 3) / 2 + 2,
					(height - View::songRowCount_) / 2);
}

GUIPoint View::GetTitlePosition() {
#ifndef PLATFORM_CAANOO
	return GUIPoint(0,0) ;
#else
	return GUIPoint(0,1) ;
#endif
} ;

bool View::Lock() {
	if (locked_) return false ;
	locked_=true ;
	return true ;
} ;

void View::WaitForObject() {
	while (locked_) {} ;
}

void View::Unlock() {
	locked_=false ;
}

void View::drawMap() {
    if (!miniLayout_) {
        GUIPoint anchor=GetAnchor() ;
		// Place the small guide/map to the bottom-right of the visible logical view
		// rather than at the left margin so it doesn't overlap main content.
		// Compute visible logical width from actual window pixel width and
		// clamp placement so the map stays inside the visible area.
		int mapWidth = 5;
		int mapHeight = 3;
		const int CHAR_PX = 8;
		int winPixelWidth = w_.GetRect().Width();
		int visibleCols = winPixelWidth / CHAR_PX;
		int width = (visibleCols > 0) ? std::min(visibleCols, LOGICAL_COLS) : LOGICAL_COLS;
		// Allow overlap: flush guide into the bottom-right corner and permit overlapping notes area
		int posX = width - mapWidth - View::margin_;
		if (posX < 0) posX = 0;
		int posY = anchor._y + View::songRowCount_ - mapHeight + 3; // push two rows further down
		if (posY < 0) posY = 0; // but never go off-screen at top
		GUIPoint pos(posX, posY);
		GUIPoint mapOrigin = pos;
    	GUITextProperties props ;

		//draw entire map (draw background without inversion so blank spaces are clear)
		SetColor(CD_HILITE1) ;
		char buffer[6] ;
		props.invert_=false ;
		//row1
		sprintf(buffer,"P G  ");
		DrawString(pos._x,pos._y,buffer,props) ;
		pos._y++ ;		
		//row2
		sprintf(buffer,"SCPIE");
		DrawString(pos._x,pos._y,buffer,props) ;
		pos._y++ ;		
		//row3
		sprintf(buffer,"  TT ");
		DrawString(pos._x,pos._y,buffer,props) ;

		// Draw static view markers (non-highlighted) so guide shows available pages
		props.invert_ = false;
		SetColor(CD_NORMAL);
		GUIPoint staticOrigin = mapOrigin;
		// Project at origin
		DrawString(staticOrigin._x, staticOrigin._y, "P", props);
		// Chain at +1,+1
		DrawString(staticOrigin._x + 1, staticOrigin._y + 1, "C", props);
		// Phrase at +2,+1
		DrawString(staticOrigin._x + 2, staticOrigin._y + 1, "P", props);
		// Instrument at +3,+1
		DrawString(staticOrigin._x + 3, staticOrigin._y + 1, "I", props);
		// Effect at +4,+1
		DrawString(staticOrigin._x + 4, staticOrigin._y + 1, "E", props);
		// Table under phrase at +2,+2
		DrawString(staticOrigin._x + 2, staticOrigin._y + 2, "T", props);
		// Table2 under instrument at +3,+2
		DrawString(staticOrigin._x + 3, staticOrigin._y + 2, "T", props);
		// Groove at +2,0
		DrawString(staticOrigin._x + 2, staticOrigin._y, "G", props);
		// Song at +0,+1 (center-left)
		DrawString(staticOrigin._x, staticOrigin._y + 1, "S", props);
		// Mixer under song (same X, Y+2)
		DrawString(staticOrigin._x, staticOrigin._y + 2, "M", props);

		//draw current screen on map (position relative to mapOrigin)
		SetColor(CD_HILITE2) ;
		pos = mapOrigin;
		switch(viewType_)
		{
		case VT_CHAIN:
			pos._x+=1;
			pos._y+=1;
	        DrawString(pos._x,pos._y,"C",props) ;
			break;
		case VT_PHRASE:
			pos._x+=2;
			pos._y+=1;
	        DrawString(pos._x,pos._y,"P",props) ;
			break;
		case VT_PROJECT:
	        DrawString(pos._x,pos._y,"P",props) ;
			break;
		case VT_INSTRUMENT:
			pos._x+=3;
			pos._y+=1;
	        DrawString(pos._x,pos._y,"I",props) ;
			break;
		case VT_EFFECT:
			pos._x+=4;
			pos._y+=1;
	        DrawString(pos._x,pos._y,"E",props) ;
			break;
		case VT_TABLE: //under phrase
			pos._x+=2;
			pos._y+=2;
	        DrawString(pos._x,pos._y,"T",props) ;
			break;
		case VT_TABLE2: //under instrument
			pos._x+=3;
			pos._y+=2;
	        DrawString(pos._x,pos._y,"T",props) ;
			break;
		case VT_GROOVE:
			pos._x+=2;
	        DrawString(pos._x,pos._y,"G",props) ;
			break;
		case VT_MIXER:
			// Place mixer marker under the Song marker (same X, lower Y)
			pos._y+=2;
			DrawString(pos._x,pos._y,"M",props) ;
			break;
		default: //VT_SONG
			pos._y+=1;
	        DrawString(pos._x,pos._y,"S",props) ;
			int foo=0;
		}

	}//!minilayout
}

void View::drawNotes() {

    if (!miniLayout_) {

		GUIPoint anchor = GetAnchor();
		// Align playing notes with song columns: start at anchor X
		// and directly below the song rows.
		int initialX = anchor._x;
		int initialY = anchor._y + View::songRowCount_;
		GUIPoint pos(initialX, initialY);
		GUITextProperties props ;

		Player *player=Player::GetInstance() ;

		//column banger refactor
		props.invert_= true;
		for (int i=0;i<SONG_CHANNEL_COUNT;i++) {
			if (i==viewData_->songX_) {
				SetColor(CD_HILITE2) ;
			} else {
				SetColor(CD_HILITE1) ;
			}
			if (player->IsRunning() && viewData_->playMode_ != PM_AUDITION) {
				DrawString(pos._x,pos._y,player->GetPlayedNote(i),props) ; //row for the note values
				pos._y++ ;
				DrawString(pos._x,pos._y,player->GetPlayedOctive(i),props) ; //row for the octive values
				pos._y++ ;
				DrawString(pos._x,pos._y,player->GetPlayedInstrument(i),props) ; //draw instrument number
			} else {
				DrawString(pos._x,pos._y,"  ",props) ; //row for the note values
				pos._y++ ;
				DrawString(pos._x,pos._y,"  ",props) ; //row for the octive values
				pos._y++ ;
				DrawString(pos._x,pos._y,"  ",props) ; //draw instrument number
			}
			pos._y = initialY ;
			pos._x+= 3;
		}
     }
}

void View::DoModal(ModalView *view,ModalViewCallback cb) {
	modalView_=view ;
	modalView_->OnFocus() ;
	modalViewCallback_=cb ;
	isDirty_=true ;
} ;

void View::Redraw() {
	if (modalView_) {
		if (isDirty_) {
			DrawView() ;
		}
		modalView_->Redraw() ;
	} else {
		DrawView() ;
	}
	isDirty_=false ;
} ;

void View::SetDirty(bool isDirty) {
	isDirty_=true ;
} ;

void View::ProcessButton(unsigned short mask, bool pressed) {
	isDirty_=false ;
	if (modalView_) {
		modalView_->ProcessButton(mask,pressed);
		modalView_->isDirty_;
		if (modalView_->IsFinished()) {
			// process callback sending the modal dialog
			if (modalViewCallback_) {
				modalViewCallback_(*this,*modalView_) ;
			}
			SAFE_DELETE(modalView_) ;
			isDirty_=true ;
		}
	} else {
		ProcessButtonMask(mask,pressed);
	}
	if (isDirty_) ((AppWindow &)w_).SetDirty() ;
} ;

void View::Clear() {
	((AppWindow &)w_).Clear() ;
}

void View::SetColor(ColorDefinition cd) {
	((AppWindow &)w_).SetColor(cd) ;
} ;

void View::ClearRect(int x,int y,int w,int h) {
	GUIRect rect(x,y,(x+w),(y+h)) ;
	w_.ClearRect(rect) ;
} ;

void View::DrawString(int x,int y,const char *txt,GUITextProperties &props) {
	GUIPoint pos(x,y) ;
	w_.DrawString(txt,pos,props) ;
} ;

/*
	Displays the saved notification for 1 second
*/
void View::EnableNotification() {
	if ((SDL_GetTicks() - notificationTime_) <= NOTIFICATION_TIMEOUT) {
		SetColor(CD_NORMAL);
		GUITextProperties props;
        int xOffset = 4;
        DrawString(xOffset, notiDistY_, displayNotification_.c_str(), props);
    } else {
		displayNotification_ = "";
	}
}

/*
    Set displayed notification
    Saves the current time
    Optionally set display y offset if not in a project (default == 2)
    Allows negative offsets, use with care!
*/
void View::SetNotification(const char *notification, int offset) {
    notificationTime_ = SDL_GetTicks();
    displayNotification_ = notification;
    notiDistY_ = offset;
    isDirty_ = true;
}
