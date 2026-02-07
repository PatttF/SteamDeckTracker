#ifndef _SOUND_FONT_MANAGER_H_
#define _SOUND_FONT_MANAGER_H_

#include "Foundation/T_Singleton.h"
#include "Externals/Soundfont/ENAB.H"
#include <vector>
#include <map>

class SoundFontManager:public T_Singleton<SoundFontManager> {
public:
	SoundFontManager() ;
	~SoundFontManager() ;
	void Reset() ;
	sfBankID LoadBank(const char *path) ;
	void UnloadBank(sfBankID id) ;
private:
	// Track sample data per bank ID so we can free on unload
	std::map<sfBankID, std::vector<void *>> bankSampleData_ ;
};
#endif
