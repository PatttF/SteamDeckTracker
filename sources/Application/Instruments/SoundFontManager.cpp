#include "SoundFontManager.h"
#include "System/System/System.h"
#include "System/FileSystem/FileSystem.h"

SoundFontManager::SoundFontManager() {
} ;

SoundFontManager::~SoundFontManager() {
} ;

void SoundFontManager::Reset() {
	// Free all sample data for all banks
	for (auto &pair : bankSampleData_) {
		for (void *p : pair.second) {
			SAFE_FREE(p) ;
		}
	}
	bankSampleData_.clear() ;
} ;

sfBankID SoundFontManager::LoadBank(const char *path) {

	sfBankID id=sfReadSFBFile((char *)path) ; 
	if (id==-1) {
		return -1 ;
	} 
	// open the file

	I_File *fin=FileSystem::GetInstance()->Open(path,"r") ;
	if (!fin) {
		return -1;
	}

	// Grab the sample offset

	long offset=sfGetSMPLOffset(id) ;

	// Grab the sample headerzz

	WORD headerCount=0 ;
	SFSAMPLEHDRPTR  &headers=sfGetSampHdrs(id,&headerCount ); 

	// Track sample allocations for this bank
	std::vector<void *> &bankSamples = bankSampleData_[id] ;

	// Loop on every sample, load them and adapt the pointers

	for (int i=0;i<headerCount;i++) {

		sfSampleHdr &current=headers[i] ;

		long from=current.dwStart*2+offset ;
		long to=current.dwEnd*2+offset ;
		
		int byteSize=to-from ;

		void *buffer=malloc(byteSize) ;

		if (buffer) {
			fin->Seek(from,SEEK_SET) ;
			fin->Read(buffer,byteSize,1) ;
		}

		// now adapt the headers so the start is the memory point
		// and all others are sample offset

		current.dwEnd=(current.dwEnd-current.dwStart) ;
		current.dwStartloop=(current.dwStartloop-current.dwStart) ;
		current.dwEndloop=(current.dwEndloop-current.dwStart) ;
        // ADDR is pointer-sized, works on both 32-bit and 64-bit
        current.dwStart = (ADDR)buffer;

        bankSamples.push_back(buffer);
    }
	fin->Close() ;
	SAFE_DELETE(fin) ;

	return id ;
} ;

void SoundFontManager::UnloadBank(sfBankID id) {
	if (id < 0) return ;

	// Free sample data for this bank
	auto it = bankSampleData_.find(id) ;
	if (it != bankSampleData_.end()) {
		for (void *p : it->second) {
			SAFE_FREE(p) ;
		}
		bankSampleData_.erase(it) ;
	}

	// Unload the bank from the ENAB layer (frees hydra + bank slot)
	sfUnloadSFBank(id) ;
} ;
