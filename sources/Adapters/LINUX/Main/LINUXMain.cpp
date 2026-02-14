#include <string.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "Adapters/LINUX/System/LINUXSystem.h"
#include "Adapters/SDL2/GUI/SDLGUIWindowImp.h"
#include "Application/Application.h"

// Global flag: set to non-zero when a VST3 component's Wine host is
// known to be stuck/dead.  When SIGABRT arrives in this state it's
// almost certainly yabridge detecting the broken connection — we want
// to survive it, not crash the tracker.
volatile sig_atomic_t g_vst3ComponentDead = 0;

static volatile sig_atomic_t s_abortCount = 0;

static void crash_handler(int sig) {
	if (sig == SIGABRT && g_vst3ComponentDead) {
		// yabridge/Wine crash after a known timeout — try to survive.
		// Re-install ourselves so abort()'s second raise is also caught.
		if (++s_abortCount <= 8) {
			signal(SIGABRT, crash_handler);
			return;  // swallow the signal
		}
		// Too many — something else is wrong, fall through to crash
	}

	const char msg_pre[] = "\n*** CRASH: signal ";
	write(STDERR_FILENO, msg_pre, sizeof(msg_pre) - 1);
	const char *signame = "???";
	if (sig == SIGSEGV) signame = "SIGSEGV";
	else if (sig == SIGABRT) signame = "SIGABRT (yabridge/Wine crash?)";
	else if (sig == SIGBUS) signame = "SIGBUS";
	else if (sig == SIGFPE) signame = "SIGFPE";
	write(STDERR_FILENO, signame, strlen(signame));
	const char msg_post[] = " ***\n";
	write(STDERR_FILENO, msg_post, sizeof(msg_post) - 1);

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
