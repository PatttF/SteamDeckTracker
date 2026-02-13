
#include "SDLEventManager.h"
#include "Application/Application.h"
#include "Application/Model/Config.h"
#include "System/Console/Trace.h"
#include "UIFramework/BasicDatas/GUIEvent.h"
#include "SDLGUIWindowImp.h"
bool SDLEventManager::finished_=false ;
bool SDLEventManager::dumpEvent_=false ;
bool SDLEventManager::capturing_=false ;
std::string SDLEventManager::capturedSource_ ;

void SDLEventManager::StartCapture() {
	capturing_ = true;
	capturedSource_.clear();
}

void SDLEventManager::StopCapture() {
	capturing_ = false;
	capturedSource_.clear();
}

bool SDLEventManager::IsCapturing() {
	return capturing_;
}

std::string SDLEventManager::ConsumeCapture() {
	std::string result = capturedSource_;
	capturedSource_.clear();
	return result;
}

SDLEventManager::SDLEventManager() 
{
}

SDLEventManager::~SDLEventManager() 
{
}

bool SDLEventManager::Init() 
{
	EventManager::Init() ;
	
	if ( SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER|SDL_INIT_TIMER) < 0 )
  {
		return false;
	}
  
	SDL_ShowCursor(SDL_DISABLE);
	
	atexit(SDL_Quit) ;
	
  SDL_InitSubSystem(SDL_INIT_JOYSTICK);
  SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);

	int joyCount=SDL_NumJoysticks() ;
	joyCount=(joyCount>MAX_JOY_COUNT)?MAX_JOY_COUNT:joyCount ;

	keyboardCS_=new KeyboardControllerSource("keyboard") ;
	const char *dumpIt=Config::GetInstance()->GetValue("DUMPEVENT") ;
	if ((dumpIt)&&(!strcmp(dumpIt,"YES")))
  {
		dumpEvent_=true ;
	}

	for (int i=0;i<MAX_JOY_COUNT;i++) 
  {
		joystick_[i]=0 ;
		gameController_[i]=0 ;
		instanceId_[i]=-1 ;
		buttonCS_[i]=0 ;
		joystickCS_[i]=0 ;
	}
    
	for (int i=0;i<joyCount;i++) 
  {
		char sourceName[128] ;

		// Try to open as a GameController first (provides consistent button
		// mapping), fall back to raw Joystick if it is not recognized.
		if (SDL_IsGameController(i)) {
			gameController_[i] = SDL_GameControllerOpen(i);
			if (gameController_[i]) {
				joystick_[i] = SDL_GameControllerGetJoystick(gameController_[i]);
				Trace::Log("EVENT","GameController[%d] opened: %s",i,
					SDL_GameControllerName(gameController_[i]));
			}
		}

		if (!joystick_[i]) {
			joystick_[i] = SDL_JoystickOpen(i);
		}

		if (joystick_[i]) {
			instanceId_[i] = SDL_JoystickInstanceID(joystick_[i]);
		}

    Trace::Log("EVENT","joystick[%d]=%x instanceId=%d",i,joystick_[i],instanceId_[i]) ;
		Trace::Log("EVENT","Number of axis:%d",SDL_JoystickNumAxes(joystick_[i])) ;
		Trace::Log("EVENT","Number of buttons:%d",SDL_JoystickNumButtons(joystick_[i])) ;
		Trace::Log("EVENT","Number of hats:%d",SDL_JoystickNumHats(joystick_[i])) ;
		snprintf(sourceName,sizeof(sourceName),"buttonJoy%d",i) ;
		buttonCS_[i]=new ButtonControllerSource(sourceName) ;
		snprintf(sourceName,sizeof(sourceName),"axisJoy%d",i) ;
		joystickCS_[i]=new JoystickControllerSource(sourceName) ;
		snprintf(sourceName,sizeof(sourceName),"hatJoy%d",i) ;
		hatCS_[i]=new HatControllerSource(sourceName) ;
	}
  
	return true ;
} 

int SDLEventManager::InstanceToIndex(SDL_JoystickID id) const {
	for (int i = 0; i < MAX_JOY_COUNT; i++) {
		if (instanceId_[i] == id) return i;
	}
	return -1;
}

void SDLEventManager::OpenDevice(int deviceIndex) {
	// Find a free slot
	int slot = -1;
	for (int s = 0; s < MAX_JOY_COUNT; s++) {
		if (!joystick_[s]) { slot = s; break; }
	}
	if (slot < 0) {
		Trace::Log("EVENT", "OpenDevice(%d): no free slot", deviceIndex);
		return;
	}

	char sourceName[128];

	if (SDL_IsGameController(deviceIndex)) {
		gameController_[slot] = SDL_GameControllerOpen(deviceIndex);
		if (gameController_[slot]) {
			joystick_[slot] = SDL_GameControllerGetJoystick(gameController_[slot]);
			Trace::Log("EVENT", "Hotplug GameController[%d] -> slot %d: %s",
				deviceIndex, slot, SDL_GameControllerName(gameController_[slot]));
		}
	}

	if (!joystick_[slot]) {
		joystick_[slot] = SDL_JoystickOpen(deviceIndex);
	}

	if (joystick_[slot]) {
		instanceId_[slot] = SDL_JoystickInstanceID(joystick_[slot]);
		Trace::Log("EVENT", "Hotplug joystick[%d] -> slot %d instanceId=%d",
			deviceIndex, slot, instanceId_[slot]);
	}

	if (!buttonCS_[slot]) {
		snprintf(sourceName, sizeof(sourceName), "buttonJoy%d", slot);
		buttonCS_[slot] = new ButtonControllerSource(sourceName);
	}
	if (!joystickCS_[slot]) {
		snprintf(sourceName, sizeof(sourceName), "axisJoy%d", slot);
		joystickCS_[slot] = new JoystickControllerSource(sourceName);
	}
	if (!hatCS_[slot]) {
		snprintf(sourceName, sizeof(sourceName), "hatJoy%d", slot);
		hatCS_[slot] = new HatControllerSource(sourceName);
	}
} 

int SDLEventManager::MainLoop() 
{
	GUIWindow *appWindow=Application::GetInstance()->GetWindow() ;
	SDLGUIWindowImp *sdlWindow=(SDLGUIWindowImp *)appWindow->GetImpWindow() ;
	while (!finished_)
	{
		SDL_Event event;

		// Block until at least one event arrives
		if (!SDL_WaitEvent(&event))
			continue;

		// Process this event and then drain ALL remaining queued events
		// before doing any expensive redraw/flush.  This prevents the
		// queue from backing up during rapid input (e.g. holding a
		// direction button) which previously caused sluggish response.
		bool needsExpose = false;
		bool needsQuit = false;

		do {
			// ---- Input events ----
			switch (event.type) {
				case SDL_KEYDOWN:
					if (dumpEvent_) 
          {
                        Trace::Log("EVENT","key(%s:%d):%d",SDL_GetScancodeName(event.key.keysym.scancode),event.key.keysym.scancode,1) ;
					}
					if (capturing_) {
						char buf[64];
						snprintf(buf, sizeof(buf), "key:0:%s",
							SDL_GetScancodeName(event.key.keysym.scancode));
						// lowercase the scancode name
						for (char *p = buf; *p; p++) {
							if (*p >= 'A' && *p <= 'Z') *p += 32;
						}
						capturedSource_ = buf;
					}
                    keyboardCS_->SetKey((int)event.key.keysym.scancode,true) ;
					break ;

				case SDL_KEYUP:
					if (dumpEvent_) 
          {
                        Trace::Log("EVENT","key(%s:%d):%d",SDL_GetScancodeName(event.key.keysym.scancode),event.key.keysym.scancode,0) ;
					}
                    keyboardCS_->SetKey((int)event.key.keysym.scancode,false) ;
					break ;


				case SDL_JOYBUTTONDOWN:
				{
					int idx = InstanceToIndex(event.jbutton.which);
					if (capturing_) {
						int capIdx = (idx >= 0) ? idx : 0;
						char buf[64];
						snprintf(buf, sizeof(buf), "but:%d:%d",
							capIdx, (int)event.jbutton.button);
						capturedSource_ = buf;
						Trace::Log("EVENT","CAPTURE joy button: %s (idx=%d instId=%d)",
							buf, idx, (int)event.jbutton.which);
					}
					if (idx >= 0 && buttonCS_[idx])
						buttonCS_[idx]->SetButton(event.jbutton.button,true) ;
					break ;
				}
				case SDL_JOYBUTTONUP:
				{
					int idx = InstanceToIndex(event.jbutton.which);
					if (dumpEvent_) {
						Trace::Log("EVENT","but(%d):%d",idx,event.jbutton.button) ;
					}
					if (idx >= 0 && buttonCS_[idx])
						buttonCS_[idx]->SetButton(event.jbutton.button,false) ;
					break ;
				}
				case SDL_JOYAXISMOTION:
				{
					int idx = InstanceToIndex(event.jaxis.which);
					if (dumpEvent_) {
						Trace::Log("EVENT","joy(%d)::%d=%d",idx,event.jaxis.axis,event.jaxis.value) ;
					}
					{
						float v = float(event.jaxis.value)/32767.0f;
						if (capturing_ && (v > 0.7f || v < -0.7f)) {
							int capIdx = (idx >= 0) ? idx : 0;
							char buf[64];
							snprintf(buf, sizeof(buf), "joy:%d:%d%c",
								capIdx, (int)event.jaxis.axis,
								v > 0 ? '+' : '-');
							capturedSource_ = buf;
							Trace::Log("EVENT","CAPTURE joy axis: %s (idx=%d instId=%d)",
								buf, idx, (int)event.jaxis.which);
						}
						if (idx >= 0 && joystickCS_[idx])
							joystickCS_[idx]->SetAxis(event.jaxis.axis, v) ;
					}
					break ;
				}
				case SDL_JOYHATMOTION:
				{
					int idx = InstanceToIndex(event.jhat.which);
					if (dumpEvent_)
          {
						for (int i=0;i<4;i++)
            {
							int mask = 1<<i ;
							if (event.jhat.value&mask)
              {
								Trace::Log("EVENT","hat(%d)::%d::%d",idx,event.jhat.hat,i) ;
							}
						}
					}
					if (capturing_ && event.jhat.value != 0) {
						for (int i = 0; i < 4; i++) {
							if (event.jhat.value & (1 << i)) {
								char buf[64];
								snprintf(buf, sizeof(buf), "hat:%d:%d",
									(int)event.jhat.hat, i);
								capturedSource_ = buf;
								Trace::Log("EVENT","CAPTURE hat: %s (idx=%d instId=%d)",
									buf, idx, (int)event.jhat.which);
								break;
							}
						}
					}
					if (idx >= 0 && hatCS_[idx])
						hatCS_[idx]->SetHat(event.jhat.hat,event.jhat.value) ;
					break ;
				}
				case SDL_JOYBALLMOTION:
					if (dumpEvent_)
          {
						Trace::Log("EVENT","ball(%d)::%d=(%d,%d)",event.jball.which,event.jball.ball,event.jball.xrel,event.jball.yrel) ;
					}
					break ;

				// --- GameController events (SteamDeck in controller mode) ---
				case SDL_CONTROLLERBUTTONDOWN:
				{
					int idx = InstanceToIndex(event.cbutton.which);
					if (dumpEvent_) {
						Trace::Log("EVENT","cbut(%d):%d down (instId=%d)",idx,(int)event.cbutton.button,(int)event.cbutton.which) ;
					}
					if (capturing_) {
						int capIdx = (idx >= 0) ? idx : 0;
						char buf[64];
						snprintf(buf, sizeof(buf), "but:%d:%d",
							capIdx, (int)event.cbutton.button);
						capturedSource_ = buf;
						Trace::Log("EVENT","CAPTURE controller button: %s (idx=%d instId=%d)",
							buf, idx, (int)event.cbutton.which);
					}
					if (idx >= 0 && buttonCS_[idx])
						buttonCS_[idx]->SetButton(event.cbutton.button, true);
					break;
				}
				case SDL_CONTROLLERBUTTONUP:
				{
					int idx = InstanceToIndex(event.cbutton.which);
					if (dumpEvent_) {
						Trace::Log("EVENT","cbut(%d):%d up (instId=%d)",idx,(int)event.cbutton.button,(int)event.cbutton.which) ;
					}
					if (idx >= 0 && buttonCS_[idx])
						buttonCS_[idx]->SetButton(event.cbutton.button, false);
					break;
				}
				case SDL_CONTROLLERAXISMOTION:
				{
					int idx = InstanceToIndex(event.caxis.which);
					float v = float(event.caxis.value) / 32767.0f;
					if (dumpEvent_) {
						Trace::Log("EVENT","caxis(%d)::%d=%d (instId=%d)",idx,(int)event.caxis.axis,event.caxis.value,(int)event.caxis.which) ;
					}
					if (capturing_ && (v > 0.7f || v < -0.7f)) {
						int capIdx = (idx >= 0) ? idx : 0;
						char buf[64];
						snprintf(buf, sizeof(buf), "joy:%d:%d%c",
							capIdx, (int)event.caxis.axis,
							v > 0 ? '+' : '-');
						capturedSource_ = buf;
						Trace::Log("EVENT","CAPTURE controller axis: %s (idx=%d instId=%d)",
							buf, idx, (int)event.caxis.which);
					}
					if (idx >= 0 && joystickCS_[idx])
						joystickCS_[idx]->SetAxis(event.caxis.axis, v);
					break;
				}

				// --- Hotplug: controllers/joysticks added after init ---
				case SDL_CONTROLLERDEVICEADDED:
				{
					int deviceIndex = event.cdevice.which;
					Trace::Log("EVENT","SDL_CONTROLLERDEVICEADDED: deviceIndex=%d", deviceIndex);
					if (InstanceToIndex(SDL_JoystickGetDeviceInstanceID(deviceIndex)) < 0) {
						OpenDevice(deviceIndex);
					}
					break;
				}
				case SDL_JOYDEVICEADDED:
				{
					int deviceIndex = event.jdevice.which;
					Trace::Log("EVENT","SDL_JOYDEVICEADDED: deviceIndex=%d", deviceIndex);
					// Only open if not already tracked (might already be opened via CONTROLLERDEVICEADDED)
					SDL_JoystickID jid = SDL_JoystickGetDeviceInstanceID(deviceIndex);
					if (jid >= 0 && InstanceToIndex(jid) < 0) {
						OpenDevice(deviceIndex);
					}
					break;
				}
				case SDL_CONTROLLERDEVICEREMOVED:
				{
					SDL_JoystickID instId = event.cdevice.which;
					int idx = InstanceToIndex(instId);
					Trace::Log("EVENT","SDL_CONTROLLERDEVICEREMOVED: instId=%d idx=%d", instId, idx);
					if (idx >= 0) {
						if (gameController_[idx]) {
							SDL_GameControllerClose(gameController_[idx]);
							gameController_[idx] = 0;
						}
						joystick_[idx] = 0;
						instanceId_[idx] = -1;
					}
					break;
				}
				case SDL_JOYDEVICEREMOVED:
				{
					SDL_JoystickID instId = event.jdevice.which;
					int idx = InstanceToIndex(instId);
					Trace::Log("EVENT","SDL_JOYDEVICEREMOVED: instId=%d idx=%d", instId, idx);
					if (idx >= 0 && !gameController_[idx]) {
						if (joystick_[idx]) {
							SDL_JoystickClose(joystick_[idx]);
						}
						joystick_[idx] = 0;
						instanceId_[idx] = -1;
					}
					break;
				}
			}

			// ---- Window / system events ----
			switch (event.type) 
			{
				case SDL_QUIT:
					needsQuit = true;
					break ;
                case SDL_WINDOWEVENT:
                    switch (event.window.event)
                    {
                        case SDL_WINDOWEVENT_EXPOSED:
                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                            needsExpose = true;
                            break;
                    }
					break ;
				case SDL_USEREVENT:
					sdlWindow->ProcessUserEvent(event) ;
					break ;
			}
		} while (SDL_PollEvent(&event));

		// Process coalesced events ONCE after draining the queue
		if (needsQuit) {
			sdlWindow->ProcessQuit();
		}
		if (needsExpose) {
			sdlWindow->ProcessExpose();
		}
	}
	return 0 ;
} ;



void SDLEventManager::PostQuitMessage()
{
  Trace::Log("EVENT","SDEM:PostQuitMessage()") ;
	finished_=true  ;
} ; 


int SDLEventManager::GetKeyCode(const char *key)
{
    return SDL_GetScancodeFromName(key);
}
