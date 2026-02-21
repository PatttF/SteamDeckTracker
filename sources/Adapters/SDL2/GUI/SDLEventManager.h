#ifndef _SDL_EVENT_MANAGER_
#define _SDL_EVENT_MANAGER_

#include <SDL2/SDL.h>
#include <string>
#include "Foundation/T_Singleton.h"
#include "Services/Controllers/ButtonControllerSource.h"
#include "Services/Controllers/HatControllerSource.h"
#include "Services/Controllers/JoystickControllerSource.h"
#include "Services/Controllers/KeyboardControllerSource.h"
#include "UIFramework/SimpleBaseClasses/EventManager.h"

#define MAX_JOY_COUNT 4



class SDLEventManager: public T_Singleton<SDLEventManager>,public EventManager {
public:
	SDLEventManager() ;
	~SDLEventManager() ;
	virtual bool Init() ;
	virtual int MainLoop() ;
	virtual void PostQuitMessage() ;
	virtual int GetKeyCode(const char *name) ;

	// Raw input capture for button mapping dialog
	static void StartCapture();
	static void StopCapture();
	static bool IsCapturing();
	// Returns the captured source string (e.g. "but:0:3") and clears it.
	// Returns empty string if nothing captured yet.
	static std::string ConsumeCapture();

private:
	// Map an SDL_JoystickID (instance ID) to our device index [0..MAX_JOY_COUNT).
	// Returns -1 if the instance ID is not recognized.
	int InstanceToIndex(SDL_JoystickID id) const;
	// Open a newly connected joystick/controller at the given device index.
	void OpenDevice(int deviceIndex);

	static bool finished_ ;
	static bool dumpEvent_ ;
	static bool capturing_ ;
	static std::string capturedSource_ ;
	SDL_Joystick *joystick_[MAX_JOY_COUNT];
	SDL_GameController *gameController_[MAX_JOY_COUNT];
	SDL_JoystickID instanceId_[MAX_JOY_COUNT]; // maps device index → instance ID
	ButtonControllerSource *buttonCS_[MAX_JOY_COUNT] ;
	JoystickControllerSource *joystickCS_[MAX_JOY_COUNT] ;
	HatControllerSource *hatCS_[MAX_JOY_COUNT] ;
	KeyboardControllerSource *keyboardCS_ ;
} ;
#endif
