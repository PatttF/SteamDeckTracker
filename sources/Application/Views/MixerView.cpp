#include "MixerView.h"
#include "Application/AppWindow.h"
#include "Adapters/SDL2/GUI/SDLGUIWindowImp.h"
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
	meterLeftX_ = 0;
	meterTopY_ = 0;
	meterDx_ = 3;
	meterGap_ = 0;
	meterHeight_ = 10;
	meterChannels_ = SONG_CHANNEL_COUNT;
	scopeLeftPx_ = 0;
	scopeTopPx_ = 0;
	scopeWidthPx_ = 0;
	scopeHeightPx_ = 0;
	memset(chPeaks_, 0, sizeof(chPeaks_));
	memset(chPeakHold_, 0, sizeof(chPeakHold_));
	memset(chPeakTimer_, 0, sizeof(chPeakTimer_));
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

	// A button — toggle mute on selected channel (rising edge only)
	// If any channel is soloed, lock out mute changes
	if ((mask & EPBM_A) && !(lastMask_ & EPBM_A)) {
		int ch = viewData_->mixerCol_;
		if (ch < SONG_CHANNEL_COUNT) {
			// Check if any channel is soloed — if so, block mute toggle
			Mixer *mx = Mixer::GetInstance();
			bool anySoloed = false;
			for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
				if (mx->IsChannelSolo(i)) { anySoloed = true; break; }
			}
			if (!anySoloed) {
				MixerService::GetInstance()->ToggleChannelMute(ch);
				PlayerMixer *pm = PlayerMixer::GetInstance();
				bool muted = pm ? pm->IsChannelMuted(ch) : false;
				char nb[32];
				snprintf(nb, sizeof(nb), "Ch %02d: %s", ch, muted ? "MUTED" : "unmuted");
				SetNotification(nb, 0);
			} else {
				SetNotification("Solo active", 0);
			}
			isDirty_ = true;
		}
	}

	// B button — toggle solo on selected channel (rising edge only)
	if ((mask & EPBM_B) && !(lastMask_ & EPBM_B)) {
		int ch = viewData_->mixerCol_;
		if (ch < SONG_CHANNEL_COUNT) {
			MixerService::GetInstance()->ToggleChannelSolo(ch);
			Mixer *mx = Mixer::GetInstance();
			bool soloed = mx ? mx->IsChannelSolo(ch) : false;
			char nb[32];
			snprintf(nb, sizeof(nb), "Ch %02d: %s", ch, soloed ? "SOLO" : "unsolo");
			SetNotification(nb, 0);
			isDirty_ = true;
		}
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

// Pixel-level meter drawing — called after text buffer flush in AppWindow::Flush()
void MixerView::DrawGraphics() {
    SDLGUIWindowImp *imp = (SDLGUIWindowImp *)w_.GetImpWindow();
    if (!imp) return;

    SDL_Renderer *renderer = imp->GetRenderer();
    if (!renderer) return;
    int mult = imp->GetMult();
    int ax = imp->GetAppAnchorX();
    int ay = imp->GetAppAnchorY();

    const int charW = 8; // pixels per char cell
    const int charH = 8;
    const int meterCharWidth = 2; // each meter is 2 chars wide in pixels
    int channels = meterChannels_;
    int dx = meterDx_;
    int gap = meterGap_;
    int mH = meterHeight_;

    // Update smoothed peaks from live audio data
    MixerService *ms = MixerService::GetInstance();
    bool clipped = ms->Clipped();

    for (int i = 0; i <= channels; i++) {
        float livePeak = 0.0f;
        if (i < channels) {
            int bus = Mixer::GetInstance()->GetBus(i);
            if (bus >= 0) {
                MixBus *mb = ms->GetMixBus(bus);
                if (mb) livePeak = mb->GetLastPeak();
            }
        } else {
            livePeak = ms->GetMasterPeak();
        }

        // Fast attack, smooth decay
        if (livePeak > chPeaks_[i]) {
            chPeaks_[i] = livePeak;
        } else {
            chPeaks_[i] *= 0.80f;
            if (chPeaks_[i] < 0.001f) chPeaks_[i] = 0.0f;
        }

        // Peak hold
        if (livePeak > chPeakHold_[i]) {
            chPeakHold_[i] = livePeak;
            chPeakTimer_[i] = 0;
        } else {
            chPeakTimer_[i]++;
            if (chPeakTimer_[i] > 30) {
                chPeakHold_[i] *= 0.96f;
                if (chPeakHold_[i] < 0.001f) chPeakHold_[i] = 0.0f;
            }
        }
    }

    // Draw pixel meters for each channel + master
    for (int i = 0; i <= channels; i++) {
        int colX = meterLeftX_ + i * (dx + gap);
        int vol;
        if (i < channels) {
            vol = ms->GetChannelVolume(i);
        } else {
            vol = ms->GetMasterVolume();
        }

        float volFrac = (float)vol / 100.0f;

        // Pixel coordinates for this meter column (inset 1px each side for gaps)
        int px = colX * charW + 1;
        int py = meterTopY_ * charH;
        int pw = meterCharWidth * charW - 2; // 14 pixels wide (inset)
        int ph = mH * charH;                 // total pixel height

        // Dark background
        SDL_SetRenderDrawColor(renderer, 0x18, 0x18, 0x18, 0xFF);
        SDL_Rect bgRect = { px * mult + ax, py * mult + ay, pw * mult, ph * mult };
        SDL_RenderFillRect(renderer, &bgRect);

        // Live audio peak with smooth gradient (bottom=green, top=red)
        float peak = chPeaks_[i];
        if (peak > 2.0f) peak = 2.0f;

        // Map -48dB..+6dB → 0.0..1.0
        float meterFrac = 0.0f;
        if (peak > 0.0001f) {
            float db = 20.0f * log10f(peak);
            const float dbMin = -48.0f;
            const float dbMax = 6.0f;
            meterFrac = (db - dbMin) / (dbMax - dbMin);
            if (meterFrac < 0.0f) meterFrac = 0.0f;
            if (meterFrac > 1.0f) meterFrac = 1.0f;
        }
        int peakPx = (int)(meterFrac * ph);
        if (peakPx > ph) peakPx = ph;

        if (peakPx > 0) {
            int meterTop = ph - peakPx;
            for (int y = meterTop; y < ph; y += 2) {
                float t = 1.0f - (float)y / (float)ph;

                int r, g, b;
                if (t < 0.65f) {
                    float s = t / 0.65f;
                    r = (int)(0x22 + s * (0xCC - 0x22));
                    g = 0xCC;
                    b = (int)(0x44 - s * 0x44);
                } else if (t < 0.85f) {
                    float s = (t - 0.65f) / 0.20f;
                    r = (int)(0xCC + s * (0xEE - 0xCC));
                    g = (int)(0xCC - s * (0xCC - 0x77));
                    b = 0x00;
                } else {
                    float s = (t - 0.85f) / 0.15f;
                    r = (int)(0xEE + s * (0xFF - 0xEE));
                    g = (int)(0x77 - s * 0x77);
                    b = 0x00;
                }
                if (clipped) { r = 0xFF; g = 0x00; b = 0x00; }

                SDL_SetRenderDrawColor(renderer, r, g, b, 0xFF);
                int h2 = (y + 2 <= ph) ? 2 : ph - y;
                SDL_Rect sr = { (px) * mult + ax, (py + y) * mult + ay, pw * mult, h2 * mult };
                SDL_RenderFillRect(renderer, &sr);
            }
        }

        // Volume level marker — thin bright line
        if (volFrac > 0.01f) {
            float volDb = 20.0f * log10f(volFrac);
            const float dbMin = -48.0f;
            const float dbMax = 6.0f;
            float volMeterFrac = (volDb - dbMin) / (dbMax - dbMin);
            if (volMeterFrac < 0.0f) volMeterFrac = 0.0f;
            if (volMeterFrac > 1.0f) volMeterFrac = 1.0f;
            int volLineY = (int)(volMeterFrac * ph);
            if (volLineY > ph - 1) volLineY = ph - 1;
            SDL_SetRenderDrawColor(renderer, 0xDD, 0xDD, 0xDD, 0xFF);
            SDL_Rect vlRect = { px * mult + ax, (py + ph - volLineY - 1) * mult + ay, pw * mult, 2 * mult };
            SDL_RenderFillRect(renderer, &vlRect);
        }

        // Peak hold marker — thin horizontal line
        float hold = chPeakHold_[i];
        if (hold > 0.01f) {
            float holdDb = 20.0f * log10f(hold);
            const float dbMin = -48.0f;
            const float dbMax = 6.0f;
            float holdFrac = (holdDb - dbMin) / (dbMax - dbMin);
            if (holdFrac < 0.0f) holdFrac = 0.0f;
            if (holdFrac > 1.0f) holdFrac = 1.0f;
            int holdPx = (int)(holdFrac * ph);
            if (holdPx > ph - 1) holdPx = ph - 1;
            GUIColor holdColor = AppWindow::cursorColor_;
            SDL_SetRenderDrawColor(renderer, holdColor._r & 0xFF, holdColor._g & 0xFF, holdColor._b & 0xFF, 0xFF);
            SDL_Rect holdRect = { px * mult + ax, (py + ph - holdPx - 2) * mult + ay, pw * mult, 2 * mult };
            SDL_RenderFillRect(renderer, &holdRect);
        }
    }

    // ── Pixel oscilloscope (M8-style waveform trace) ──
    if (scopeWidthPx_ > 0 && scopeHeightPx_ > 0) {
        int sx = scopeLeftPx_;
        int sy = scopeTopPx_;
        int sw = scopeWidthPx_;
        int sh = scopeHeightPx_;

        // Use theme background color for scope area
        SDL_SetRenderDrawColor(renderer,
            AppWindow::backgroundColor_._r & 0xFF,
            AppWindow::backgroundColor_._g & 0xFF,
            AppWindow::backgroundColor_._b & 0xFF, 0xFF);
        SDL_Rect scopeBgRect = { sx * mult + ax, sy * mult + ay, sw * mult, sh * mult };
        SDL_RenderFillRect(renderer, &scopeBgRect);

        // Subtle center line
        int centerY = sy + sh / 2;
        int clR = (AppWindow::backgroundColor_._r & 0xFF) + 0x10;
        int clG = (AppWindow::backgroundColor_._g & 0xFF) + 0x14;
        int clB = (AppWindow::backgroundColor_._b & 0xFF) + 0x16;
        if (clR > 255) clR = 255;
        if (clG > 255) clG = 255;
        if (clB > 255) clB = 255;
        SDL_SetRenderDrawColor(renderer, clR, clG, clB, 0xFF);
        SDL_Rect clRect = { sx * mult + ax, centerY * mult + ay, sw * mult, 1 * mult };
        SDL_RenderFillRect(renderer, &clRect);

        // Read oscilloscope samples from MixerService
        MixerService *ms2 = MixerService::GetInstance();
        int scopeN = ms2->GetOscilloscopeSampleCount();
        if (scopeN > 0) {
            const int traceR1 = 0x66, traceG1 = 0xDD, traceB1 = 0xFF; // cyan
            const int traceR2 = 0xCC, traceG2 = 0x66, traceB2 = 0xFF; // purple

            int prevY = -1;
            for (int x = 0; x < sw; x++) {
                int sIdx = (x * scopeN) / sw;
                if (sIdx >= scopeN) sIdx = scopeN - 1;
                float sample = ms2->GetOscilloscopeSample(sIdx);

                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;

                int usableH = sh - 4;
                int midY = sy + sh / 2;
                int curY = midY - (int)(sample * (usableH / 2));

                float t = (float)x / (float)(sw > 1 ? sw - 1 : 1);
                int r = traceR1 + (int)(t * (traceR2 - traceR1));
                int g = traceG1 + (int)(t * (traceG2 - traceG1));
                int b = traceB1 + (int)(t * (traceB2 - traceB1));

                if (prevY >= 0) {
                    int y0 = (prevY < curY) ? prevY : curY;
                    int y1 = (prevY < curY) ? curY : prevY;
                    if (y1 - y0 < 1) y1 = y0 + 1;

                    // Dim glow
                    SDL_SetRenderDrawColor(renderer, r / 4, g / 4, b / 4, 0xFF);
                    SDL_Rect glowRect = { (sx + x - 1) * mult + ax, (y0 - 1) * mult + ay,
                                          3 * mult, (y1 - y0 + 3) * mult };
                    SDL_RenderFillRect(renderer, &glowRect);

                    // Bright trace
                    SDL_SetRenderDrawColor(renderer, r, g, b, 0xFF);
                    SDL_Rect traceRect = { (sx + x) * mult + ax, y0 * mult + ay,
                                           1 * mult, (y1 - y0 + 1) * mult };
                    SDL_RenderFillRect(renderer, &traceRect);
                }
                prevY = curY;
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

	// Shift everything up by 1 row so meters start at pos._y - 1
	int meterStartRow = pos._y - 1;

	for (int i = 0; i < channels; i++) {
		int colX = pos._x; // base x for this column

		// Prepare per-column props so label/meter/hex/percent all share the same highlight
		GUITextProperties colProps = props;
		bool selected = (i == viewData_->mixerCol_);
		colProps.invert_ = selected;
		SetColor(selected ? CD_HILITE2 : CD_NORMAL);

		// label above meter (shifted up 1 row)
		int bus = Mixer::GetInstance()->GetBus(i);
		hex2char(bus, hex);
		DrawString(colX, meterStartRow - 1, hex, colProps);

		// Reserve blank space for the meter area — DrawGraphics() paints pixel bars
		{
			char empty[8];
			int ew = (dx < (int)sizeof(empty)-1) ? dx : (int)sizeof(empty)-1;
			for (int e = 0; e < ew; e++) empty[e] = ' ';
			empty[ew] = '\0';
			SetColor(CD_NORMAL);
			for (int m = 0; m < meterHeight; m++) {
				DrawString(colX, meterStartRow + m, empty, props);
			}
		}

		// draw volume percent right below meter
		int vol = MixerService::GetInstance()->GetChannelVolume(i);
		char vstr[8];
		snprintf(vstr, sizeof(vstr), "%3d", vol);
		DrawString(colX, meterStartRow + meterHeight, vstr, props);

		// draw mute/solo indicator
		{
			PlayerMixer *pm = PlayerMixer::GetInstance();
			Mixer *mx = Mixer::GetInstance();
			bool muted = pm ? pm->IsChannelMuted(i) : false;
			bool soloed = mx ? mx->IsChannelSolo(i) : false;
			char ms[4];
			if (soloed) {
				snprintf(ms, sizeof(ms), " S ");
				SetColor(CD_CURSOR);
			} else if (muted) {
				snprintf(ms, sizeof(ms), " M ");
				SetColor(CD_HILITE1);
			} else {
				snprintf(ms, sizeof(ms), "   ");
				SetColor(CD_NORMAL);
			}
			// Only draw as many chars as dx allows
			ms[dx < 3 ? dx : 3] = '\0';
			DrawString(colX, meterStartRow + meterHeight + 1, ms, props);
		}

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
	DrawString(colX, meterStartRow - 1, lblm, colProps);

	// Reserve blank space for master meter — DrawGraphics() paints pixel bars
	{
		char empty[8];
		int ew = (dx < (int)sizeof(empty)-1) ? dx : (int)sizeof(empty)-1;
		for (int e = 0; e < ew; e++) empty[e] = ' ';
		empty[ew] = '\0';
		SetColor(CD_NORMAL);
		for (int m = 0; m < meterHeight; m++) {
			DrawString(colX, meterStartRow + m, empty, props);
		}
	}

	// draw volume percent for master right below meter
	int masterVol = MixerService::GetInstance()->GetMasterVolume();
	char vstrm[8];
	snprintf(vstrm, sizeof(vstrm), "%3d", masterVol);
	DrawString(colX, meterStartRow + meterHeight, vstrm, props);

	// master has no mute/solo, draw blank in that row
	{
		char blank[4] = "   ";
		blank[dx < 3 ? dx : 3] = '\0';
		SetColor(CD_NORMAL);
		DrawString(colX, meterStartRow + meterHeight + 1, blank, props);
	}

	// Cache layout for DrawGraphics pixel meter rendering
	meterLeftX_ = mixerLeftX;
	meterTopY_ = meterStartRow;  // meters shifted up 1 row
	meterDx_ = dx;
	meterGap_ = gap;
	meterHeight_ = meterHeight;
	meterChannels_ = channels;

	// Reserve blank space for pixel oscilloscope (drawn in DrawGraphics)
	// Layout: meters | vol% | mute/solo | oscilloscope
	{
		const int vizHeight = 9;
		int vizRow = meterStartRow + meterHeight + 2;
		int totalCols = (channels + 1) * dx + channels * gap;
		if (totalCols <= 0) totalCols = 1;

		// Clear character cells so no text artifacts show through
		std::string blank(totalCols, ' ');
		SetColor(CD_NORMAL);
		for (int r = 0; r < vizHeight; r++) {
			DrawString(mixerLeftX, vizRow + r, blank.c_str(), props);
		}

		// Cache pixel coordinates for DrawGraphics oscilloscope rendering
		scopeLeftPx_ = mixerLeftX * 8;
		scopeTopPx_ = vizRow * 8;
		scopeWidthPx_ = totalCols * 8;
		scopeHeightPx_ = vizHeight * 8;
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
