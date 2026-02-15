#include "LINUXSystem.h"
#include <libgen.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string>
#include "System/Console/Trace.h"
#include "Adapters/SDL2/GUI/GUIFactory.h"
#include "Adapters/SDL2/GUI/SDLEventManager.h"
#include "Adapters/SDL2/GUI/SDLGUIWindowImp.h"
#include "Adapters/SDL2/Timer/SDLTimer.h"
#include "Adapters/Unix/FileSystem/UnixFileSystem.h"
#include "Adapters/Unix/Process/UnixProcess.h"
#include "Application/Controllers/ControlRoom.h"
#include "Application/Commands/NodeList.h"
#include "Application/Model/Config.h"
#include "System/Console/Logger.h"

#ifdef DUMMYMIDI
#include "Adapters/Dummy/Midi/DummyMidi.h"
#endif

#ifdef JACKAUDIO
#include "Adapters/Jack/Audio/JackAudio.h"
#include "Adapters/Jack/Client/JackClient.h"
#endif

#ifdef JACKMIDI
#include "Adapters/Jack/Midi/JackMidiService.h"
#endif

#ifdef RTAUDIO
#include "Adapters/RTAudio/RTAudioStub.h"
#endif

#ifdef RTMIDI
#include "Adapters/RTMidi/RTMidiService.h"
#endif

#ifdef SDLAUDIO
#include "Adapters/SDL2/Audio/SDLAudio.h"
#endif

EventManager *LINUXSystem::eventManager_ = NULL;
static int secbase = 0;

// Create directories recursively (best-effort)
static int mkdir_recursive(const std::string &path) {
	if (path.empty()) return 0;
	struct stat st;
	if (stat(path.c_str(), &st) == 0) return 0; // already exists
	size_t pos = path.find_last_of('/');
	if (pos != std::string::npos) {
		std::string parent = path.substr(0, pos);
		if (!parent.empty()) mkdir_recursive(parent);
	}
	if (mkdir(path.c_str(), 0755) != 0) {
		if (errno == EEXIST) return 0;
		return -1;
	}
	return 0;
}

/*
 * starts the main loop
 */
int LINUXSystem::MainLoop() {
	eventManager_->InstallMappings();
	return eventManager_->MainLoop() ;
};

/*
 * initializes the application
 */
void LINUXSystem::Boot(int argc,char **argv) {

    SDL_setenv((char *)"SDL_VIDEO_X11_WMCLASS",(char *)"SDTracker",1) ;

	// Set LV2_PATH to include all common plugin directories so lilv finds everything
	{
		std::string lv2Path;
		const char *existing = getenv("LV2_PATH");
		if (existing && existing[0]) {
			lv2Path = existing;
			lv2Path += ":";
		}
		const char *h = getenv("HOME");
		if (h) {
			lv2Path += std::string(h) + "/.lv2:";
		}
		lv2Path += "/usr/lib/lv2";
		lv2Path += ":/usr/local/lib/lv2";
		lv2Path += ":/usr/lib/x86_64-linux-gnu/lv2";
		lv2Path += ":/usr/lib64/lv2";
		setenv("LV2_PATH", lv2Path.c_str(), 1);
	}

	// Install System
	System::Install(new LINUXSystem());

	// Install FileSystem
	FileSystem::Install(new UnixFileSystem());

	// Install aliases
	char buff[1024];
	ssize_t len = ::readlink("/proc/self/exe",buff,sizeof(buff)-1);
	if (len != -1)
	{
		buff[len] = 0;
	}
	else
	{
		strcpy(buff,".");
	}
	Path::SetAlias("bin",dirname(buff)) ;
	// Point external resources to $HOME/Documents/SDTracker
	// Use HOME if set, otherwise fallback to current directory
	const char *home = getenv("HOME");
	if (home) {
			std::string sdPath = std::string(home) + "/Documents/SDTracker";
			Path::SetAlias("root", sdPath.c_str());

			// Ensure the SDTracker folder and key subfolders exist: projects, samplelib
			mkdir_recursive(sdPath);
			mkdir_recursive(sdPath + "/projects");
			mkdir_recursive(sdPath + "/samplelib");
			mkdir_recursive(sdPath + "/themes");
	} else {
			Path::SetAlias("root",".");
	}

  // Set up TeeLogger: writes to both stdout and a log file
  {
    std::string logPath;
    if (home) {
      logPath = std::string(home) + "/Documents/SDTracker/sdtracker.log";
    } else {
      logPath = "sdtracker.log";
    }
    TeeLogger *tee = new TeeLogger(logPath.c_str());
    Trace::GetInstance()->SetLogger(*tee);
    // Also point the crash handler's fd at the log file
    extern int g_crashLogFd;
    g_crashLogFd = tee->GetFd();
  }

  // Process arguments
  Config::GetInstance()->ProcessArguments(argc,argv) ;

  // Install GUI Factory
  I_GUIWindowFactory::Install(new GUIFactory()) ;

  // Install Timers
  TimerService::GetInstance()->Install(new SDLTimerService()) ;

#ifdef JACKAUDIO
	
	Audio::Install(new JackAudio(AudioSettings hints));
#endif

#ifdef RTAUDIO
	
	AudioSettings hints ;
	hints.bufferSize_= 256 ;
	hints.preBufferCount_=2 ;
	Audio::Install(new RTAudioStub(hints)) ;
#endif

#ifdef SDLAUDIO
	
	AudioSettings hint;
	hint.bufferSize_ = 512;
	hint.preBufferCount_ = 2;
	Audio::Install(new SDLAudio(hint));
#endif

#ifdef DUMMYMIDI
	
	MidiService::Install(new DummyMidi());
#endif

#ifdef JACKMIDI
	
  MidiService::Install(new JackMidiService()) ;
#endif

#ifdef RTMIDI
	
	MidiService::Install(new RTMidiService()) ;
#endif

	// Install Threads
	SysProcessFactory::Install(new UnixProcessFactory());

	// ── Audio environment setup ─────────────────────────────────────────
	// Must happen BEFORE SDL_Init(SDL_INIT_AUDIO) so SDL's audio subsystem
	// initialization can find PipeWire / PulseAudio sockets.
	//
	// On the Steam Deck in Gaming Mode, XDG_RUNTIME_DIR is often not set
	// when launching from a .desktop shortcut or Steam itself, which makes
	// PipeWire and PulseAudio unreachable for SDL.
	{
		const char *xdgRuntimeDir = getenv("XDG_RUNTIME_DIR") ;
		if (!xdgRuntimeDir || xdgRuntimeDir[0] == '\0') {
			char runtimeBuf[256] ;
			snprintf(runtimeBuf, sizeof(runtimeBuf), "/run/user/%d", (int)getuid()) ;
			struct stat st ;
			if (stat(runtimeBuf, &st) == 0 && S_ISDIR(st.st_mode)) {
				setenv("XDG_RUNTIME_DIR", runtimeBuf, 0) ;
				
				xdgRuntimeDir = getenv("XDG_RUNTIME_DIR") ;
			}
		}

		// If SDL_AUDIODRIVER is not already forced, pick the best available
		// sound server.  PipeWire is native on SteamOS / Steam Deck;
		// PulseAudio is its compat layer and works everywhere else.
		if (!getenv("SDL_AUDIODRIVER") && xdgRuntimeDir && xdgRuntimeDir[0]) {
			char socketBuf[512] ;
			struct stat st ;

			// Check for PipeWire socket
			snprintf(socketBuf, sizeof(socketBuf), "%s/pipewire-0", xdgRuntimeDir) ;
			if (stat(socketBuf, &st) == 0) {
				SDL_setenv("SDL_AUDIODRIVER", "pipewire", 0) ;
				
			} else {
				// Check for PulseAudio socket
				snprintf(socketBuf, sizeof(socketBuf), "%s/pulse/native", xdgRuntimeDir) ;
				if (stat(socketBuf, &st) == 0) {
					SDL_setenv("SDL_AUDIODRIVER", "pulseaudio", 0) ;
					
				}
			}
		}
	}

	if ( SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_TIMER) < 0 ) {
		return;
	}
	SDL_ShowCursor(SDL_DISABLE);

	atexit(SDL_Quit);

	eventManager_ = I_GUIWindowFactory::GetInstance()->GetEventManager();
	eventManager_ -> Init();
};

void LINUXSystem::Shutdown() {};

/*
 * get current time for status display
 */
unsigned long LINUXSystem::GetClock() {
	struct timeval tp;

	gettimeofday(&tp, NULL);
	if (!secbase) {
		secbase = tp.tv_sec;
		return long(tp.tv_usec/1000.0);
	}
	return long((tp.tv_sec - secbase)*1000 + tp.tv_usec/1000.0);
}

/*
 * wraps sleep, guess we never sleep!
 */
void LINUXSystem::Sleep(int millisec) {
	//if (millisec>0)
	//::Sleep(millisec) ;
}

/*
 * wraps malloc
 */
void *LINUXSystem::Malloc(unsigned size) {
	void *ptr=malloc(size) ;
	//
	return ptr ;
}

/*
 * wraps free
 */
void LINUXSystem::Free(void *ptr) {
	free(ptr);
} 

/*
 * wraps memset
 */
void LINUXSystem::Memset(void *addr,char val,int size) {
#ifdef _64BIT
    unsigned int ad = (intptr_t)addr;
#else
    unsigned int ad=(unsigned int)addr;
#endif
    if (((ad&0x3)==0)&&((size&0x3)==0)) { // Are we 4-byte aligned ?
        unsigned int intVal=0;
        for (int i=0;i<4;i++) {
             intVal=(intVal<<8)+val;
        }
        unsigned int *dst=(unsigned int *)addr;
        size_t intSize=size>>2 ;

        for (unsigned int i=0;i<intSize;i++) {
            *dst++=intVal ;
        }
    } else {
        memset(addr,val,size) ;
    };
};

/*
 * wraps memcpy
 */
void *LINUXSystem::Memcpy(void *s1, const void *s2, int n) {
    return memcpy(s1,s2,n) ;
};

/*
 * logprint
 */
void LINUXSystem::AddUserLog(const char *msg) {
	fprintf(stderr,"LOG: %s\n",msg) ;
};

/*
 * print after quit
 */
void LINUXSystem::PostQuitMessage() {
	SDLEventManager::GetInstance()->PostQuitMessage() ;
}; 

/*
 * get memory usage, guess it's infinite
 */
unsigned int LINUXSystem::GetMemoryUsage() { return 0; };
