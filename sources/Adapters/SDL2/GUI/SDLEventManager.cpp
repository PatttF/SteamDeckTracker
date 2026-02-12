
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
	
	if ( SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK|SDL_INIT_TIMER) < 0 )
  {
		return false;
	}
  
	SDL_ShowCursor(SDL_DISABLE);
	
	atexit(SDL_Quit) ;
	
  SDL_InitSubSystem(SDL_INIT_JOYSTICK);

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
		buttonCS_[i]=0 ;
		joystickCS_[i]=0 ;
	}
    
	for (int i=0;i<joyCount;i++) 
  {
		char sourceName[128] ;
		joystick_[i]=SDL_JoystickOpen(i) ;
    Trace::Log("EVENT","joystick[%d]=%x",i,joystick_[i]) ;
		Trace::Log("EVENT","Number of axis:%d",SDL_JoystickNumAxes(joystick_[i])) ;
		Trace::Log("EVENT","Number of buttons:%d",SDL_JoystickNumButtons(joystick_[i])) ;
		Trace::Log("EVENT","Number of hats:%d",SDL_JoystickNumHats(joystick_[i])) ;
		sprintf(sourceName,"buttonJoy%d",i) ;
		buttonCS_[i]=new ButtonControllerSource(sourceName) ;
		sprintf(sourceName,"axisJoy%d",i) ;
		joystickCS_[i]=new JoystickControllerSource(sourceName) ;
		sprintf(sourceName,"hatJoy%d",i) ;
		hatCS_[i]=new HatControllerSource(sourceName) ;
	}
  
	return true ;
} 

int SDLEventManager::MainLoop() 
{
	GUIWindow *appWindow=Application::GetInstance()->GetWindow() ;
	SDLGUIWindowImp *sdlWindow=(SDLGUIWindowImp *)appWindow->GetImpWindow() ;
	while (!finished_)
	{
		SDL_Event event;
		if (SDL_WaitEvent(&event)) 
    {
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
					if (capturing_) {
						char buf[64];
						snprintf(buf, sizeof(buf), "but:%d:%d",
							(int)event.jbutton.which, (int)event.jbutton.button);
						capturedSource_ = buf;
					}
					if (event.jbutton.which < MAX_JOY_COUNT && buttonCS_[event.jbutton.which])
						buttonCS_[event.jbutton.which]->SetButton(event.jbutton.button,true) ;
					break ;
				case SDL_JOYBUTTONUP:
					if (dumpEvent_) {
						Trace::Log("EVENT","but(%d):%d",event.button.which,event.jbutton.button) ;
					}
					if (event.jbutton.which < MAX_JOY_COUNT && buttonCS_[event.jbutton.which])
						buttonCS_[event.jbutton.which]->SetButton(event.jbutton.button,false) ;
					break ;
				case SDL_JOYAXISMOTION:
					if (dumpEvent_) {
						Trace::Log("EVENT","joy(%d)::%d=%d",event.jaxis.which,event.jaxis.axis,event.jaxis.value) ;
					}
					if (capturing_) {
						float v = float(event.jaxis.value)/32767.0f;
						if (v > 0.7f || v < -0.7f) {
							char buf[64];
							snprintf(buf, sizeof(buf), "joy:%d:%d%c",
								(int)event.jaxis.which, (int)event.jaxis.axis,
								v > 0 ? '+' : '-');
							capturedSource_ = buf;
						}
					}
					if (event.jaxis.which < MAX_JOY_COUNT && joystickCS_[event.jaxis.which])
						joystickCS_[event.jaxis.which]->SetAxis(event.jaxis.axis,float(event.jaxis.value)/32767.0f) ;
					break ;
				case SDL_JOYHATMOTION:
					if (dumpEvent_)
          {
						for (int i=0;i<4;i++)
            {
							int mask = 1<<i ;
							if (event.jhat.value&mask)
              {
								Trace::Log("EVENT","hat(%d)::%d::%d",event.jhat.which,event.jhat.hat,i) ;
							}
						}
					}
					if (capturing_ && event.jhat.value != 0) {
						// Find which direction bit is set (SDL hat: 1=up,2=right,4=down,8=left)
						// Map to our hat channel: bit0=up, bit1=right, bit2=down, bit3=left
						for (int i = 0; i < 4; i++) {
							if (event.jhat.value & (1 << i)) {
								char buf[64];
								snprintf(buf, sizeof(buf), "hat:%d:%d",
									(int)event.jhat.hat, i);
								capturedSource_ = buf;
								break;
							}
						}
					}
					if (event.jhat.which < MAX_JOY_COUNT && hatCS_[event.jhat.which])
						hatCS_[event.jhat.which]->SetHat(event.jhat.hat,event.jhat.value) ;
					break ;
				case SDL_JOYBALLMOTION:
					if (dumpEvent_)
          {
						Trace::Log("EVENT","ball(%d)::%d=(%d,%d)",event.jball.which,event.jball.ball,event.jball.xrel,event.jball.yrel) ;
					}
					break ;
		}

			switch (event.type) 
			{

				case SDL_QUIT:
					sdlWindow->ProcessQuit() ;
					break ;
                case SDL_WINDOWEVENT:
                    switch (event.window.event)
                    {
                        case SDL_WINDOWEVENT_EXPOSED:
                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                            sdlWindow->ProcessExpose() ;
                            break;
                    }
					break ;
				case SDL_USEREVENT:
					sdlWindow->ProcessUserEvent(event) ;
					break ;
			}
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
