#ifndef _INSTRUMENT_BANK_H_
#define _INSTRUMENT_BANK_H_

#include "Application/Persistency/Persistent.h"
#include "Application/Model/Song.h"
#include "Application/Instruments/I_Instrument.h"

#define NO_MORE_INSTRUMENT 0x100

class InstrumentBank: public Persistent {
public:
	InstrumentBank() ;
	~InstrumentBank() ;
	void AssignDefaults() ;
	I_Instrument *GetInstrument(int i) ;
	virtual void SaveContent(TiXmlNode *node);
	virtual void RestoreContent(TiXmlElement *element);
	void Init() ;
	void OnStart() ;
	unsigned short GetNext() ;
	unsigned short Clone(unsigned short i) ;
	// Change the type of the instrument at index 'i' to 'type', preserving matching variables where possible
	void SetInstrumentType(int i, InstrumentType type);
private:
	I_Instrument *instrument_[MAX_INSTRUMENT_COUNT] ;

	// Stash for per-instrument MIDI input assignment so we can preserve
	// IMDI/IMIC across temporary type switches (example: VST3 -> AudioIn -> VST3)
	int savedIMDI_[MAX_INSTRUMENT_COUNT];
	int savedIMIC_[MAX_INSTRUMENT_COUNT];
} ;

#endif
