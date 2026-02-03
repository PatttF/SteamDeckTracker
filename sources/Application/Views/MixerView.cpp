#include "MixerView.h"
#include "Application/AppWindow.h"
#include "Application/LogicalSize.h"
#include "Application/Model/Mixer.h"
#include "Application/Utils/char.h"
#include "Application/Mixer/MixerService.h"
#include "Application/Player/PlayerMixer.h"
#include <string>
#include <iostream>
#include <sstream>
#include <stdlib.h>
#include <cmath>
#include <vector>
#include <SDL.h>
#include "Services/Audio/Audio.h"
#include "System/Console/Trace.h"

MixerView::MixerView(GUIWindow &w,ViewData *viewData):View(w,viewData) {
	clipboard_.active_=false ;
	clipboard_.data_=0 ;
	invertBatt_=false;
	lastMask_ = 0;
	lastUpRepeatTime_ = 0;
	lastDownRepeatTime_ = 0;
	lastDrawTick_ = SDL_GetTicks();
	lastUpdateLogTick_ = 0;
	lastEventPushTick_ = 0;
	// Observe MixerService updates so we can request redraws on audio buffer events
	MixerService::GetInstance()->AddObserver(*this);
}

MixerView::~MixerView() {
	// Clean up observer registration
	MixerService::GetInstance()->RemoveObserver(*this);
}
 


void MixerView::onStart() {
	Player *player=Player::GetInstance() ;
	unsigned char from=viewData_->songX_ ;
	unsigned char to=from ;
	//if (clipboard_.active_) {
	//	GUIRect r=getSelectionRect();
	//	from=r.Left() ;
	//	to=r.Right() ;
	//}
	player->OnStartButton(PM_SONG,from,false,to) ;
} ;

void MixerView::onStop() {
	Player *player=Player::GetInstance() ;
	unsigned char from=viewData_->songX_ ;
	unsigned char to=from ;
	player->OnStartButton(PM_SONG,from,true,to) ;
} ;

void MixerView::OnFocus() {
	// OnFocus: no debug logging
} ;

void MixerView::Update(Observable &o, I_ObservableData *d) {
	// Received notification from MixerService — request redraw on the main thread
	unsigned int now = SDL_GetTicks();
	isDirty_ = true;
	((AppWindow &)w_).SetDirty();

	// Post a GUI event into the main loop so the window processes the update on the main thread.
	// Throttle event pushes to avoid flooding the main loop (min interval ~30ms)
	const unsigned int EVENT_PUSH_MIN_MS = 30;
	if ((now - lastEventPushTick_) >= EVENT_PUSH_MIN_MS) {
		GUIEvent ev(0, ET_PLAYERUPDATE, now);
		w_.PushEvent(ev);
		lastEventPushTick_ = now;
	}

	// Maintain a low-frequency diagnostic tick counter (kept for debug builds only)
	if ((int)(now - lastUpdateLogTick_) > 500) {
		lastUpdateLogTick_ = now;
	}
} ;


void MixerView::updateCursor(int dx,int dy) {
	int x = viewData_->mixerCol_;
	x += dx;
	if (x < 0) x = 0;
	// Allow selecting an extra column for the master strip (index = SONG_CHANNEL_COUNT)
	if (x > SONG_CHANNEL_COUNT) x = SONG_CHANNEL_COUNT;
	viewData_->mixerCol_ = x;
	isDirty_ = true;
}

void MixerView::ProcessButtonMask(unsigned short mask,bool pressed) {
	//if (!pressed) {
	//	if (viewMode_==VM_MUTEON) {
	//		if (mask&EPBM_R) {
	//			toggleMute() ;
	//		}
	//	} ;
	//	if (viewMode_==VM_SOLOON) {
	//		if (mask&EPBM_R) {
	//			switchSoloMode() ;
	//		}
	//	} ;
	//	return ;
	//} ;
	//
	
	if (clipboard_.active_) {
		viewMode_=VM_SELECTION ;
	} ;
	// Process selection related keys
	
	if (viewMode_==VM_SELECTION) {
        if (clipboard_.active_==false) {
            clipboard_.active_=true ;
            clipboard_.x_=viewData_->songX_ ;
            clipboard_.y_=viewData_->songY_ ;
            clipboard_.offset_=viewData_->songOffset_ ;
			saveX_=clipboard_.x_ ;
			saveY_=clipboard_.y_ ;
			saveOffset_=clipboard_.offset_ ;
        }
        processSelectionButtonMask(mask, pressed) ;
    } else {
       
       // Switch back to normal mode

        viewMode_=VM_NORMAL ;
        processNormalButtonMask(mask, pressed) ;
    }
} ;


/******************************************************
 processNormalButtonMask:
        process button mask in the case there is no
        selection active
 ******************************************************/
 
void MixerView::processNormalButtonMask(unsigned int mask, bool pressed) {

	// Back: support both sequences: (R rising while UP held) OR (UP rising while R held).
	// This makes both "press W then Up" and "press Up while holding W" work.
	if ((mask & EPBM_R) && (mask & EPBM_UP) && ( (!(lastMask_ & EPBM_R)) || (!(lastMask_ & EPBM_UP)) )) {
		ViewType vt = VT_SONG;
		ViewEvent ve(VET_SWITCH_VIEW, &vt);
		SetChanged();
		NotifyObservers(&ve);
		// don't return — allow other rising-edge handling to run
	}

	// Navigation
	if (mask & EPBM_LEFT)
		updateCursor(-1, 0);
	if (mask & EPBM_RIGHT)
		updateCursor(1, 0);

	// START (play/stop) — rising edge
	if ((mask & EPBM_START) && !(lastMask_ & EPBM_START)) {
		Player *player = Player::GetInstance();
		if (player->IsRunning()) {
			onStop();
		} else {
			onStart();
		}
		isDirty_ = true;
	}

	// Volume up/down — support immediate action on rising edge and auto-repeat while held
	const unsigned int REPEAT_INITIAL_MS = 400; // ms before auto-repeat starts
	const unsigned int REPEAT_INTERVAL_MS = 60; // ms between repeats while held
	unsigned int now = SDL_GetTicks();

	// UP handling
	if (mask & EPBM_UP) {
		if (!(lastMask_ & EPBM_UP)) {
			// rising edge — immediate change
			int ch = viewData_->mixerCol_;
			if (ch < SONG_CHANNEL_COUNT) {
				int vol = MixerService::GetInstance()->GetChannelVolume(ch);
				MixerService::GetInstance()->SetChannelVolume(ch, vol + 1);
				char nb[32];
				int newVol = MixerService::GetInstance()->GetChannelVolume(ch);
				snprintf(nb, sizeof(nb), "Ch %02d: %3d%%", ch, newVol);
				SetNotification(nb, 0);
			} else {
				// master
				int mvol = MixerService::GetInstance()->GetMasterVolume();
				MixerService::GetInstance()->SetMasterVolume(mvol + 1);
				// Persist to project so PlayerMixer won't overwrite on next update
				if (viewData_ && viewData_->project_) {
					Variable *v = viewData_->project_->FindVariable(VAR_MASTERVOL);
					if (v) v->SetInt(MixerService::GetInstance()->GetMasterVolume(), true);
				}
				int newVol = MixerService::GetInstance()->GetMasterVolume();
				char nb[32];
				snprintf(nb, sizeof(nb), "MAS %3d%%", newVol);
				SetNotification(nb, 0);
			}
			isDirty_ = true;

			((AppWindow &)w_).SetDirty();
			lastUpRepeatTime_ = now + REPEAT_INITIAL_MS;
		} else {
			// held — check repeat timer
			if (now >= lastUpRepeatTime_) {
				int ch = viewData_->mixerCol_;
				if (ch < SONG_CHANNEL_COUNT) {
					int vol = MixerService::GetInstance()->GetChannelVolume(ch);

					MixerService::GetInstance()->SetChannelVolume(ch, vol + 1);
					char nb[32];
					int newVol = MixerService::GetInstance()->GetChannelVolume(ch);
					snprintf(nb, sizeof(nb), "Ch %02d: %3d%%", ch, newVol);
					SetNotification(nb, 0);
				} else {
					int mvol = MixerService::GetInstance()->GetMasterVolume();
					MixerService::GetInstance()->SetMasterVolume(mvol + 1);
					if (viewData_ && viewData_->project_) {
						Variable *v = viewData_->project_->FindVariable(VAR_MASTERVOL);
						if (v) v->SetInt(MixerService::GetInstance()->GetMasterVolume(), true);
					}
					int newVol = MixerService::GetInstance()->GetMasterVolume();
					char nb[32];
					snprintf(nb, sizeof(nb), "MAS %3d%%", newVol);
					SetNotification(nb, 0);
				}

				isDirty_ = true;
				((AppWindow &)w_).SetDirty();

				lastUpRepeatTime_ = now + REPEAT_INTERVAL_MS;
			}
		}
	} else {
		// not pressed — reset repeat timer so next press has full initial delay
		lastUpRepeatTime_ = 0;
	}

	// DOWN handling
	if (mask & EPBM_DOWN) {
		if (!(lastMask_ & EPBM_DOWN)) {
			// rising edge — immediate change
			int ch = viewData_->mixerCol_;
			if (ch < SONG_CHANNEL_COUNT) {
				int vol = MixerService::GetInstance()->GetChannelVolume(ch);
				MixerService::GetInstance()->SetChannelVolume(ch, vol - 1);
				char nb[32];
				int newVol = MixerService::GetInstance()->GetChannelVolume(ch);
				snprintf(nb, sizeof(nb), "Ch %02d: %3d%%", ch, newVol);
				SetNotification(nb, 0);
			} else {
				int mvol = MixerService::GetInstance()->GetMasterVolume();
				MixerService::GetInstance()->SetMasterVolume(mvol - 1);
				if (viewData_ && viewData_->project_) {
					Variable *v = viewData_->project_->FindVariable(VAR_MASTERVOL);
					if (v) v->SetInt(MixerService::GetInstance()->GetMasterVolume(), true);
				}
				int newVol = MixerService::GetInstance()->GetMasterVolume();
				char nb[32];
				snprintf(nb, sizeof(nb), "MAS %3d%%", newVol);
				SetNotification(nb, 0);
			}
			isDirty_ = true;
			((AppWindow &)w_).SetDirty();
			lastDownRepeatTime_ = now + REPEAT_INITIAL_MS;
		} else {
			// held — check repeat timer
			if (now >= lastDownRepeatTime_) {
				int ch = viewData_->mixerCol_;
				if (ch < SONG_CHANNEL_COUNT) {
					int vol = MixerService::GetInstance()->GetChannelVolume(ch);

					MixerService::GetInstance()->SetChannelVolume(ch, vol - 1);
					char nb[32];
					int newVol = MixerService::GetInstance()->GetChannelVolume(ch);
					snprintf(nb, sizeof(nb), "Ch %02d: %3d%%", ch, newVol);
					SetNotification(nb, 0);
				} else {
					int mvol = MixerService::GetInstance()->GetMasterVolume();
					MixerService::GetInstance()->SetMasterVolume(mvol - 1);

					int newVol = MixerService::GetInstance()->GetMasterVolume();
					char nb[32];
					snprintf(nb, sizeof(nb), "MAS %3d%%", newVol);
					SetNotification(nb, 0);
				}
				isDirty_ = true;
				((AppWindow &)w_).SetDirty();
				lastDownRepeatTime_ = now + REPEAT_INTERVAL_MS;
			}
		}
	} else {
		lastDownRepeatTime_ = 0;
	}

	// store mask for edge detection
	lastMask_ = mask;
} ;

/******************************************************
 processSelectionButtonMask:
        process button mask in the case there is a
        selection active
 ******************************************************/
 
void MixerView::processSelectionButtonMask(unsigned int mask, bool pressed) {

	// B Modifier

	if (mask & EPBM_B) {

    } else {

	  // A modifier

	  if (mask & EPBM_A) {

	  } else {

		  // R Modifier

          	if (mask & EPBM_R) {
				if ((mask & EPBM_START) && pressed) {
			    	onStop() ;
                }
	    	} else {

				// No modifier
		       		if ((mask & EPBM_START) && pressed) {
					   onStart() ;
	    			}
		    }
	  } 
	}
}

void MixerView::DrawView() {

	Clear();

	GUITextProperties props;
	GUIPoint pos = GetTitlePosition();
	GUIPoint anchor = GetAnchor();
	char hex[3];

	SetColor(CD_NORMAL);

	// Title shows the current view name on the mixer page
	DrawString(pos._x, pos._y, "Mixer", props);

	Player *player = Player::GetInstance();

	// Support dynamic channel count (use SONG_CHANNEL_COUNT)
	int channels = SONG_CHANNEL_COUNT;
	pos = anchor;
	// Left-align the mixer block to the far-left of the screen so labels don't get clipped
	pos._x = 0;

	// Choose spacing dynamically so we have room for 3-digit volume display when possible.
	// Compute maximum per-channel width that still fits on screen, clamped to 3..4 (we need min 3 to show 100)
	int maxDX = (LOGICAL_COLS - 6) / channels;
	if (maxDX < 3) maxDX = 3;
	if (maxDX > 4) maxDX = 4;
	short dx = (short)maxDX;
	const int gap = 0; // no gap between channels (tighter layout)

	const int meterHeight = 10; // number of character rows for fader (increased to 10 per user request)

	// Ensure the whole mixer block fits on screen; include an extra column for the master strip and shift left if needed
	int totalWidth = (channels + 1) * dx + channels * gap;
	if (pos._x + totalWidth + 6 > LOGICAL_COLS) {
		pos._x = LOGICAL_COLS - totalWidth - 6;
		if (pos._x < 0) pos._x = 0;
	}
	// store left-most X for the mixer block so we can draw aligned overlays later
	int mixerLeftX = pos._x;
	// Diagnostic logging removed to avoid per-frame trace spam
	// (layout values remain available for debug builds if needed)


	// Draw channel labels and faders
	unsigned int now = SDL_GetTicks();

	for (int i = 0; i < channels; i++) {
		int colX = pos._x; // base x for this column

		// Prepare per-column props so label/meter/hex/percent all share the same highlight
		GUITextProperties colProps = props;
		bool selected = (i == viewData_->mixerCol_);
		colProps.invert_ = selected;
		SetColor(selected ? CD_HILITE2 : CD_NORMAL);

		// label above meter: show bus id (hex) to match the bottom label
		int bus = Mixer::GetInstance()->GetBus(i);
		hex2char(bus, hex);
		DrawString(colX, pos._y - 1, hex, colProps);

		// draw meter as stacked blocks (bottom = lowest)
		// meter should be 2 chars wide for the visual style
		int meterWidth = 2;
		// prepare padding strings (dx up to 4)
		char padStr[8] = "";
		int pad = dx - meterWidth;
		if (pad > 0) {
			for (int p = 0; p < pad && p < (int)sizeof(padStr)-1; p++) padStr[p] = ' ';
			padStr[pad] = '\0';
		} else {
			padStr[0] = '\0';
		}

		// Display volume-only meter: read channel volume (0..100) and map to meter height
		int channelVol = MixerService::GetInstance()->GetChannelVolume(i);
		float displayPeak = (float)channelVol / 100.0f;
		int filled = int(displayPeak * meterHeight + 0.5f);
		if (filled < 0) filled = 0;
		if (filled > meterHeight) filled = meterHeight;


		for (int m = 0; m < meterHeight; m++) {
			int row = meterHeight - 1 - m; // bottom-up
			// prepare a pad string for remaining column width and a small filled block for the meter area
			char padStr[8] = "";
			int pad = dx - meterWidth;
			if (pad > 0) {
				for (int p = 0; p < pad && p < (int)sizeof(padStr)-1; p++) padStr[p] = ' ';
				padStr[pad] = '\0';
			} else {
				padStr[0] = '\0';
			}
			// filled block (foreground glyphs) — keep it simple and robust
			char fillBlock[8];
			int fw = (meterWidth < (int)sizeof(fillBlock)-1) ? meterWidth : (int)sizeof(fillBlock)-1;
			for (int e = 0; e < fw; e++) fillBlock[e] = '#';
			fillBlock[fw] = '\0';

			if (row < filled) {
				// pick darker color by default, switch to lighter when channel has recent audio activity
				float mbPeak = 0.0f;
				if (bus >= 0) {
					MixBus *mb = MixerService::GetInstance()->GetMixBus(bus);
					if (mb) mbPeak = mb->GetLastPeak();
				}
				// show clip color only when master output is actually clipping
                if (MixerService::GetInstance()->Clipped()) SetColor(CD_CLIP);
                else SetColor(mbPeak > 0.02f ? CD_HILITE2 : CD_HILITE1);
				// draw the filled glyphs and then pad to the full column width
				DrawString(colX, pos._y + m, fillBlock, props);
				if (pad > 0) DrawString(colX + meterWidth, pos._y + m, padStr, props);
			} else {
				// empty row: draw spaces across the column
				char empty[8];
				int ew = (dx < (int)sizeof(empty)-1) ? dx : (int)sizeof(empty)-1;
				for (int e = 0; e < ew; e++) empty[e] = ' ';
				empty[ew] = '\0';
				DrawString(colX, pos._y + m, empty, props);
			}
		}

		// draw empty row below the meter to create a gap between meter and volume percent
		{
			char empty[8];
			int ew = (dx < (int)sizeof(empty)-1) ? dx : (int)sizeof(empty)-1;
			for (int e = 0; e < ew; e++) empty[e] = ' ';
			empty[ew] = '\0';
			DrawString(colX, pos._y + meterHeight, empty, props);
		}

		// draw volume percent (left-align within column so three digits are visible)
		int vol = MixerService::GetInstance()->GetChannelVolume(i);
		char vstr[8];
		snprintf(vstr, sizeof(vstr), "%3d", vol);
		DrawString(colX, pos._y + meterHeight + 1, vstr, props);

	

		pos._x = colX + dx + gap;
	}

	// update last draw tick so decay uses correct dt
	lastDrawTick_ = now;

	// Draw master as an additional mixer strip to the right of the channel columns
	Audio *audio = Audio::GetInstance();
	int colX = pos._x; // pos is already incremented after the last channel
	// label for master (three chars 'MAS') and selection highlight
	bool selectedMaster = (viewData_->mixerCol_ == channels);
	GUITextProperties colProps = props;
	colProps.invert_ = selectedMaster;
	SetColor(selectedMaster ? CD_HILITE2 : CD_NORMAL);
	char lblm[4];
	snprintf(lblm, sizeof(lblm), "MAS");
	DrawString(colX, pos._y - 1, lblm, colProps);

	// Master meter (same height as channel meters)
	int masterVol = MixerService::GetInstance()->GetMasterVolume();
	float masterPeak = (float)masterVol / 100.0f;
	int masterFilled = int(masterPeak * meterHeight + 0.5f);
	if (masterFilled < 0) masterFilled = 0;
	if (masterFilled > meterHeight) masterFilled = meterHeight;
	for (int m = 0; m < meterHeight; m++) {
		int row = meterHeight - 1 - m;
		int pad = dx - 2;
		if (row < masterFilled) {
			float mp = MixerService::GetInstance()->GetMasterPeak();
			// show clip color only when output driver flags actual clipping
                if (MixerService::GetInstance()->Clipped()) SetColor(CD_CLIP);
                else SetColor(mp > 0.02f ? CD_HILITE2 : CD_HILITE1);
			// draw the filled glyphs and then pad to the full column width
			DrawString(colX, pos._y + m, "##", props);
			if (pad > 0) {
				char padStr[8] = "";
				for (int p = 0; p < pad && p < (int)sizeof(padStr)-1; p++) padStr[p] = ' ';
				padStr[pad] = '\0';
				DrawString(colX + 2, pos._y + m, padStr, props);
			}
		} else {
			char empty[8];
			int ew = (dx < (int)sizeof(empty)-1) ? dx : (int)sizeof(empty)-1;
			for (int e = 0; e < ew; e++) empty[e] = ' ';
			empty[ew] = '\0';
			DrawString(colX, pos._y + m, empty, props);
		}
	}

	// draw volume percent for master (percent not inverted)
	char vstrm[8];
	snprintf(vstrm, sizeof(vstrm), "%3d", masterVol);
	DrawString(colX, pos._y + meterHeight + 1, vstrm, props);

	// Enhanced waveform visualizer: taller, smoothed, with gradient glyphs
	{
		const int vizHeight = 9; // odd so we have a center row
		int vizRow = pos._y + meterHeight + 2; // top row of waveform
		int totalCols = (channels + 1) * dx + channels * gap; // character columns available for waveform
		int sampleCount = MixerService::GetInstance()->GetWaveSampleCount();

		if (totalCols <= 0) totalCols = 1;

		// prepare rows using std::string to avoid manual malloc/free
		std::vector<std::string> rows;
		rows.resize(vizHeight);
		for (int r = 0; r < vizHeight; r++) rows[r].assign(totalCols, ' ');

		int center = vizHeight / 2;

		// Draw columns from oldest (left) to newest (right) with simple linear interpolation
		for (int c = 0; c < totalCols; c++) {
			float rel = (float)c / (float)(totalCols - 1);
			float fidx = rel * (sampleCount - 1);
			int idx = (int)floor(fidx);
			float frac = fidx - idx;
			float s1 = MixerService::GetInstance()->GetWaveSample(idx);
			float s2 = MixerService::GetInstance()->GetWaveSample((idx + 1) % sampleCount);
			float s = s1 * (1.0f - frac) + s2 * frac; // interpolated amplitude [0..1]
			// small smoothing: average with neighboring samples to reduce flicker
			if (idx > 0) s = (s + MixerService::GetInstance()->GetWaveSample(idx - 1) * 0.5f) / 1.5f;

			if (s < 0.0f) s = 0.0f;
			if (s > 1.0f) s = 1.0f;

			int amp = int(s * (float)center + 0.5f);
			int top = center - amp;
			int bottom = center + amp;
			if (top < 0) top = 0;
			if (bottom >= vizHeight) bottom = vizHeight - 1;

			for (int r = top; r <= bottom; r++) {
				// choose glyph by proximity to center for a gradient look
				float d = 1.0f - (fabs((float)center - (float)r) / (float)center);
				char g;
				if (d >= 0.85f) g = '#';
				else if (d >= 0.6f) g = '=';
				else if (d >= 0.35f) g = '-';
				else g = '.';
				rows[r][c] = g;
			}

			// if very low amplitude and center is empty, draw a subtle baseline
			if (amp == 0 && rows[center][c] == ' ') rows[center][c] = '-';
		}

		// render rows; use highlight for activity
		for (int r = 0; r < vizHeight; r++) {
			SetColor(CD_HILITE1);
			DrawString(mixerLeftX, vizRow + r, rows[r].c_str(), props);
		}
	}

	drawMap();
	drawNotes();

	// Keep the mixer view updating while the player is running so meters remain responsive.
	// This sets the app dirty flag; actual redraws are processed by the main event loop.
	if (player->IsRunning()) {
		OnPlayerUpdate(PET_UPDATE);
		((AppWindow &)w_).SetDirty();
	}
};

void MixerView::OnPlayerUpdate(PlayerEventType ,unsigned int tick) {

	Player *player=Player::GetInstance() ;

	// Draw clipping indicator & CPU usage

	GUIPoint anchor=GetAnchor() ;
	GUIPoint pos=anchor ;

	GUITextProperties props ;
	SetColor(CD_NORMAL) ;

// Place playing info at the bottom-left so it doesn't compete with the guide/map (flush with content left margin)
	pos = anchor;
	pos._x = 0; // far left corner (flush)
	// reserve 2 lines for playing info (battery + time)
	int infoLines = 2;
	pos._y = anchor._y + View::songRowCount_ - infoLines + 4; // push down to overlap similar to Song view
	if (player->Clipped()) {
		SetColor(CD_CLIP);
		DrawString(pos._x, pos._y, "clip", props);
		pos._y += 1;
	}

	char strbuffer[32];
	System *sys = System::GetInstance();
	int batt = sys->GetBatteryLevel();
	if (batt >= 0) {
		if (batt < 90) {
			SetColor(CD_HILITE2);
			invertBatt_ = !invertBatt_;
		} else {
			invertBatt_ = false;
		};
		props.invert_ = invertBatt_;

		pos._y += 1;
		snprintf(strbuffer, sizeof(strbuffer), "%3.3d", batt);
		DrawString(pos._x, pos._y, strbuffer, props);
	}
	SetColor(CD_NORMAL);
	props.invert_ = false;
	int time = int(player->GetPlayTime());
	int mi = time / 60;
	int se = time - mi * 60;
	snprintf(strbuffer, sizeof(strbuffer), "%2.2d:%2.2d", mi, se);
	pos._y += 1;
	DrawString(pos._x, pos._y, strbuffer, props);

} ;
