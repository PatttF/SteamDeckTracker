
#include "UnixProcess.h"
#include "System/Console/Trace.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

static int s_semCounter = 0;

void *_UnixStartThread(void *p) {
	SysThread *play=(SysThread *)p ;
	play->startExecution() ;
	return NULL ;
}

bool UnixProcessFactory::BeginThread(SysThread& thread) {
pthread_t pthread ;

	pthread_create(&pthread,0,_UnixStartThread,&thread) ;
	return true ;
}

SysSemaphore *UnixProcessFactory::CreateNewSemaphore(int initialcount, int maxcount) {
	return new UnixSysSemaphore(initialcount,maxcount) ;
} ;

UnixSysSemaphore::UnixSysSemaphore(int initialcount,int maxcount) {
	// Each instance needs its own named semaphore — the old code used a
	// single shared name which caused all semaphores to alias each other.
	snprintf(semName_, sizeof(semName_), "/lgpt_sem_%d_%d",
	         (int)getpid(), s_semCounter++);
	sem_=sem_open(semName_,O_CREAT|O_EXCL,S_IRUSR|S_IWUSR, 0);
	if (sem_==SEM_FAILED) {
		// If it already exists (stale from a crash), unlink and retry
		sem_unlink(semName_);
		sem_=sem_open(semName_,O_CREAT|O_EXCL,S_IRUSR|S_IWUSR, 0);
	}
	maxcount_=(maxcount>0)?maxcount:0;
} ;

UnixSysSemaphore::~UnixSysSemaphore() {
  sem_close(sem_) ;
  sem_unlink(semName_) ;
} ;

SysSemaphoreResult UnixSysSemaphore::Wait() {
	sem_wait(sem_) ;
	return SSR_NO_ERROR ;
} ;

SysSemaphoreResult UnixSysSemaphore::TryWait() {
	return SSR_INVALID ;
}

SysSemaphoreResult UnixSysSemaphore::WaitTimeout(unsigned long timeout) {
	return SSR_INVALID ;
} ;

SysSemaphoreResult UnixSysSemaphore::Post() {
	// Enforce max count to prevent unbounded semaphore flooding.
	// When the audio producer thread stalls (e.g. yabridge IPC),
	// the SDL callback keeps posting.  Without a cap the
	// semaphore value grows to hundreds, causing a burst of
	// AddBuffer calls that permanently fills the pool.
	if (maxcount_>0) {
		int val=0;
		if (sem_getvalue(sem_,&val)==0 && val>=maxcount_) {
			return SSR_NO_ERROR; // silently drop
		}
	}
	sem_post(sem_) ;
	return SSR_NO_ERROR ;
} ;
