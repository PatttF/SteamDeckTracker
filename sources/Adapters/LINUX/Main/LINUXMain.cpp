#include <string.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <stdlib.h>
#include "Adapters/LINUX/System/LINUXSystem.h"
#include "Adapters/SDL2/GUI/SDLGUIWindowImp.h"
#include "Application/Application.h"

static void crash_handler(int sig) {
	const char msg[] = "\n*** CRASH: signal received ***\n";
	write(STDERR_FILENO, msg, sizeof(msg) - 1);

	void *frames[64];
	int n = backtrace(frames, 64);
	backtrace_symbols_fd(frames, n, STDERR_FILENO);

	_exit(128 + sig);
}

/*
 * generic entrypoint for linux based targets
 */
int main(int argc,char *argv[]) {
	signal(SIGSEGV, crash_handler);
	signal(SIGABRT, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGFPE, crash_handler);
	LINUXSystem::Boot(argc,argv);

	SDLCreateWindowParams params;
	params.title="SDTracker";
	params.cacheFonts_=true;

	Application::GetInstance()->Init(params);

	return LINUXSystem::MainLoop();
}

void _assert() {};
