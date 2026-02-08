
#include "InstrumentBank.h"
#include "Application/Instruments/SampleInstrument.h"
#include "Application/Instruments/SamplePool.h"
#include "Application/Instruments/MidiInstrument.h"
#include "Application/Instruments/LV2Instrument.h"
#include "Application/Instruments/SoundFontInstrument.h"
#include "Application/Instruments/VST3Instrument.h"
#include "Application/Player/PlayerMixer.h"
#include "System/io/Status.h"
#include "Application/Utils/char.h"
#include "Application/Model/Config.h"
#include "Application/Persistency/PersistencyService.h"
#include "Filters.h"
#include "Foundation/Variables/WatchedVariable.h"
#include <ctype.h>

char *InstrumentTypeData[IT_LAST]= {
	"Sample",
	"Midi",
	"LV2",
	"SF2",
	"VST3"
} ;


// Contain all instrument definition

InstrumentBank::InstrumentBank():Persistent("INSTRUMENTBANK") {

   	for (int i=0;i<MAX_SAMPLEINSTRUMENT_COUNT;i++) {
        SampleInstrument *s=new SampleInstrument() ;
        instrument_[i]=s ;
    }
	for (int i=0;i<MAX_MIDIINSTRUMENT_COUNT;i++) {
        MidiInstrument *s=new MidiInstrument() ;
        s->SetChannel(i) ;
        instrument_[MAX_SAMPLEINSTRUMENT_COUNT+i]=s ;
    }
	for (int i=0;i<MAX_LV2INSTRUMENT_COUNT;i++) {
        LV2Instrument *s=new LV2Instrument() ;
        instrument_[MAX_SAMPLEINSTRUMENT_COUNT+MAX_MIDIINSTRUMENT_COUNT+i]=s ;
    }
	for (int i=0;i<MAX_SOUNDFONTINSTRUMENT_COUNT;i++) {
        SoundFontInstrument *s=new SoundFontInstrument() ;
        instrument_[MAX_SAMPLEINSTRUMENT_COUNT+MAX_MIDIINSTRUMENT_COUNT+MAX_LV2INSTRUMENT_COUNT+i]=s ;
    }
	for (int i=0;i<MAX_VST3INSTRUMENT_COUNT;i++) {
        VST3Instrument *s=new VST3Instrument() ;
        instrument_[MAX_SAMPLEINSTRUMENT_COUNT+MAX_MIDIINSTRUMENT_COUNT+MAX_LV2INSTRUMENT_COUNT+MAX_SOUNDFONTINSTRUMENT_COUNT+i]=s ;
    }

    // Insert a 'type' Variable into each instrument so the UI can bind to it and allow switching
    static char *instrTypes[] = { (char*)"Sample", (char*)"LV2", (char*)"SF2", (char*)"VST3" } ;
    for (int i = 0; i < MAX_INSTRUMENT_COUNT; ++i) {
        I_Instrument *ins = instrument_[i];
        // ID: ITYP
        FourCC id = MAKE_FOURCC('I','T','Y','P');
        int init = 0;
        if (ins->GetType() == IT_LV2) init = 1;
        else if (ins->GetType() == IT_SOUNDFONT) init = 2;
        else if (ins->GetType() == IT_VST3) init = 3;
        WatchedVariable *tv = new WatchedVariable("type", id, instrTypes, 4, init);
        ins->Insert(tv);
    }

    Status::Set("All instrument loaded") ;
}

// Assigns default instruments value for new project
//

void InstrumentBank::AssignDefaults() {

	SamplePool *pool=SamplePool::GetInstance() ;
   	for (int i=0;i<MAX_SAMPLEINSTRUMENT_COUNT;i++) {
		SampleInstrument *s=(SampleInstrument*)instrument_[i] ;
		if (i<pool->GetNameListSize()) {
	        s->AssignSample(i) ;
		} else {
			s->AssignSample(-1) ;
		} 
    } ;
} ;

InstrumentBank::~InstrumentBank() {
	for (int i=0;i<MAX_INSTRUMENT_COUNT;i++) {
		delete instrument_[i] ;
	}	
} ;

I_Instrument *InstrumentBank::GetInstrument(int i) {
	if (i < 0 || i >= MAX_INSTRUMENT_COUNT) {
		return instrument_[0] ; // Return first instrument as fallback
	}
	return instrument_[i] ;
} ;

void InstrumentBank::SaveContent(TiXmlNode *node) {
	char hex[3] ;
	for (int i=0;i<MAX_INSTRUMENT_COUNT;i++) {

		I_Instrument *instr=instrument_[i] ;
		if (!instr->IsEmpty()) {
			TiXmlElement data("INSTRUMENT") ;
			hex2char(i,hex) ;
			data.SetAttribute("ID",hex) ;
			data.SetAttribute("TYPE",InstrumentTypeData[instr->GetType()]) ;

			IteratorPtr<Variable> it(instr->GetIterator()) ;
			// For LV2 instruments, ensure parameter Variables exist and are
			// synchronized from the instrument's internal parameter state so
			// they will be serialized reliably.
			if (instr->GetType() == IT_LV2) {
				LV2Instrument *lin = (LV2Instrument *)instr;
				int pc = lin->GetParameterCount();
				for (int pi = 0; pi < pc; ++pi) {
					const LV2PluginParameter *p = lin->GetParameter(pi);
					if (!p) continue;
					char varName[64];
					snprintf(varName, sizeof(varName), "p%d", pi);
					// compute scaled 0-127 value from parameter currentValue
					int scaled = 0;
					if (p->maxValue > p->minValue) {
						float normalized = (p->currentValue - p->minValue) / (p->maxValue - p->minValue);
						scaled = int(normalized * 127.0f + 0.5f);
						if (scaled < 0) scaled = 0; if (scaled > 127) scaled = 127;
					}
					Variable *v = instr->FindVariable(varName);
					if (v) {
						v->SetInt(scaled, false);
					} else {
						Variable *nv = new Variable(varName, MAKE_FOURCC('L','P',pi/256,pi%256), scaled);
						instr->Insert(nv);
					}
				}
			}

			int count=0 ;
			for (it->Begin();!it->IsDone();it->Next()) {
				Variable &v=it->CurrentItem() ;
				TiXmlElement param("PARAM") ;
				param.SetAttribute("NAME",v.GetName()) ;
				param.SetAttribute("VALUE",v.GetString()) ;
				data.InsertEndChild(param) ;
				count++ ;
			}

			// For SoundFont instruments, save the sf2 file path and preset index
			if (instr->GetType() == IT_SOUNDFONT) {
				SoundFontInstrument *sfi = (SoundFontInstrument *)instr;
				const char *sf2path = sfi->GetSF2Path();
				if (sf2path && sf2path[0]) {
					TiXmlElement paramp("PARAM");
					paramp.SetAttribute("NAME", "sf2file");
					paramp.SetAttribute("VALUE", sf2path);
					data.InsertEndChild(paramp);
					count++;
				}
				int pi = sfi->GetCurrentPreset();
				if (pi >= 0) {
					char buf[16]; snprintf(buf, sizeof(buf), "%d", pi);
					TiXmlElement paramp("PARAM");
					paramp.SetAttribute("NAME", "preset");
					paramp.SetAttribute("VALUE", buf);
					data.InsertEndChild(paramp);
					count++;
				}
			}
			// VST3 instruments: save plugin path, class ID, and full state blobs
			if (instr->GetType() == IT_VST3) {
				VST3Instrument *vin = (VST3Instrument *)instr;
				int pc = vin->GetParameterCount();
				for (int pi = 0; pi < pc; ++pi) {
					const VST3PluginParameter *p = vin->GetParameter(pi);
					if (!p) continue;
					char varName[64];
					snprintf(varName, sizeof(varName), "v3p%d", pi);
					int scaled = (int)(p->currentValue * 255.0 + 0.5);
					if (scaled < 0) scaled = 0; if (scaled > 255) scaled = 255;
					Variable *v = instr->FindVariable(varName);
					if (v) {
						v->SetInt(scaled, false);
					} else {
						Variable *nv = new Variable(varName, MAKE_FOURCC('V','P',pi/256,pi%256), scaled);
						instr->Insert(nv);
					}
				}
				// Save full component state blob (base64)
				std::string compState = vin->GetComponentStateBase64();
				if (!compState.empty()) {
					TiXmlElement paramp("PARAM");
					paramp.SetAttribute("NAME", "vst3_comp_state");
					paramp.SetAttribute("VALUE", compState.c_str());
					data.InsertEndChild(paramp);
					count++;
				}
				// Save full controller state blob (base64)
				std::string ctrlState = vin->GetControllerStateBase64();
				if (!ctrlState.empty()) {
					TiXmlElement paramp("PARAM");
					paramp.SetAttribute("NAME", "vst3_ctrl_state");
					paramp.SetAttribute("VALUE", ctrlState.c_str());
					data.InsertEndChild(paramp);
					count++;
				}
			}
			// Ensure LV2 plugin URI and parameter values are recorded even if
			// Variables weren't present for some reason (fallback persistence).
			if (instr->GetType() == IT_LV2) {
				LV2Instrument *lin = (LV2Instrument *)instr;
				// Plugin URI
				const char *puri = lin->GetPluginURI();
				if (puri && puri[0]) {
					TiXmlElement paramp("PARAM");
					paramp.SetAttribute("NAME", "plugin");
					paramp.SetAttribute("VALUE", puri);
					data.InsertEndChild(paramp);
					count++;
				}
				// Parameters: prefer to save Variable values if available, otherwise
				// save the parameter currentValue scaled to 0-127 to match UI scaling.
				int pc = lin->GetParameterCount();
				for (int pi = 0; pi < pc; ++pi) {
					const LV2PluginParameter *p = lin->GetParameter(pi);
					if (!p) continue;
					char varName[64];
					snprintf(varName, sizeof(varName), "p%d", pi);
					// Try to find the Variable on the instrument
					Variable *v = lin->FindVariable(varName);
					std::string val;
					if (v) {
						val = v->GetString();
					} else {
						// scale currentValue to 0-127
						float normalized = 0.0f;
						if (p->maxValue > p->minValue)
							normalized = (p->currentValue - p->minValue) / (p->maxValue - p->minValue);
						int scaled = int(normalized * 127.0f + 0.5f);
						if (scaled < 0) scaled = 0; if (scaled > 127) scaled = 127;
						char buf[16]; snprintf(buf, sizeof(buf), "%d", scaled);
						val = buf;
					}
					TiXmlElement paramp("PARAM");
					paramp.SetAttribute("NAME", varName);
					paramp.SetAttribute("VALUE", val.c_str());
					data.InsertEndChild(paramp);
					count++;
				}
			}
			if (count) node->InsertEndChild(data) ;
		}
	}
} ;

void InstrumentBank::RestoreContent(TiXmlElement *element) {

	TiXmlElement *current=element->FirstChildElement() ;

	PersistencyDocument *doc=(PersistencyDocument *)element->GetDocument() ;
  if (doc->version_ < 130)
  {
    if (Config::GetInstance()->GetValue("LEGACYDOWNSAMPLING") != NULL)
    {
      SampleInstrument::EnableDownsamplingLegacy();
    }
  }
	while (current) {

		// Check it is an instrument
		
		if (!strcmp(current->Value(),"INSTRUMENT")) {

			// Get the instrument ID
			
			const char* hexid=current->Attribute("ID") ;
			if (!hexid || strlen(hexid) < 2) {
				current=current->NextSiblingElement() ;
				continue ;
			}
			unsigned char b1=(c2h__(hexid[0]))<<4 ;
			unsigned char b2=c2h__(hexid[1]) ;
			unsigned char id=b1+b2 ;			

			InstrumentType it=IT_LAST ;
			const char* instype=current->Attribute("TYPE") ;
			if (instype) {
				for (int i=0;i<IT_LAST;i++) {
					if (!strcmp(instype,InstrumentTypeData[i])) {
						it=(InstrumentType)i ;
						break ;
					}
				}
			} else {
				it=(id<MAX_SAMPLEINSTRUMENT_COUNT)?IT_SAMPLE:IT_MIDI ;
			} ;

			// If TYPE wasn't provided in older project files, sniff through params for
			// LV2 markers (plugin URI or p# parameter names) or SF2 markers and convert
			// those instruments to the right type.
			if (instype == NULL) {
				TiXmlElement *guess = current->FirstChildElement();
				while (guess) {
					const char *gname = guess->Attribute("NAME");
					const char *gval = guess->Attribute("VALUE");
					if (gname) {
						if (!strcmp(gname, "sf2file")) { it = IT_SOUNDFONT; break; }
						if (!strcmp(gname, "plugin")) { it = IT_LV2; break; }
						if (gname[0] == 'p' && isdigit((unsigned char)gname[1])) { it = IT_LV2; break; }
						// VST3 params are named v3p0, v3p1, etc.
						if (gname[0]=='v' && gname[1]=='3' && gname[2]=='p') { it = IT_VST3; break; }
					}
					if (gval) {
						if (strstr(gval, ".vst3")) { it = IT_VST3; break; }
						if (strstr(gval, "://") || strstr(gval, ".lv2") || strchr(gval, '/')) { it = IT_LV2; break; }
					}
					guess = guess->NextSiblingElement();
				}
			}

			if (id<MAX_INSTRUMENT_COUNT) {
        I_Instrument *instr=instrument_[id] ;
				if (instr->GetType()!=it) {
					delete instr ;
					switch (it) {
						case IT_SAMPLE:
							instr=new SampleInstrument() ;
							break ;
						case IT_MIDI:
							instr=new MidiInstrument() ;
							break ;
						case IT_LV2:
							instr=new LV2Instrument() ;
							break ;
						case IT_SOUNDFONT:
							instr=new SoundFontInstrument() ;
							break ;
						case IT_VST3:
							instr=new VST3Instrument() ;
							break ;					default:
						instr=new SampleInstrument() ;
						break ;					}
					instrument_[id]=instr ;
				// Ensure 'type' Variable exists on restored/replaced instruments so the UI binds correctly
				int typeInit = 0;
				if (it==IT_LV2) typeInit = 1;
				else if (it==IT_SOUNDFONT) typeInit = 2;
				else if (it==IT_VST3) typeInit = 3;
				Variable *tv = instr->FindVariable(MAKE_FOURCC('I','T','Y','P'));
				if (tv) {
					tv->SetInt(typeInit, false);
				} else {
					static char *instrTypes[] = { (char*)"Sample", (char*)"LV2", (char*)"SF2", (char*)"VST3" } ;
					WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 4, typeInit);
					instr->Insert(wtv);
				} 
			}

        TiXmlElement *param=current->FirstChildElement() ;
				while (param) {
					const char *name=param->Attribute("NAME") ;
					const char *value=param->Attribute("VALUE") ;

					if (!name || !value) {
						param=param->NextSiblingElement() ;
						continue ;
					}

          // Convert old filter dist to newer filter mode

          if (!strcmp(name,"filter dist"))
          {
            name = "filter mode";
            if (!strcmp(value,"none"))
            {
              value = "original";
            }
            else
            {
              value = "scream";
            }
          }

					IteratorPtr<Variable> it(instr->GetIterator()) ;
					bool applied=false;
					for (it->Begin();!it->IsDone();it->Next()) {
						Variable &v=it->CurrentItem() ;
						if (!strcmp(v.GetName(),name)) {
							v.SetString(value) ;
							applied=true;
							break;
						}
					}
					if (!applied) {
						// If this is an LV2 instrument, store the param for later when
						// parameters are discovered during Init()
						if (instr->GetType()==IT_LV2) {
							LV2Instrument *lin = (LV2Instrument *)instr;
							lin->StorePendingVariable(name,value);
						}
						// SoundFont instruments store sf2file and preset for Init()
						if (instr->GetType()==IT_SOUNDFONT) {
							SoundFontInstrument *sfi = (SoundFontInstrument *)instr;
							sfi->StorePendingVariable(name,value);
						}
						// VST3 instruments store params for Init()
						if (instr->GetType()==IT_VST3) {
							VST3Instrument *vin = (VST3Instrument *)instr;
							vin->StorePendingVariable(name,value);
						}
					}
					param=param->NextSiblingElement() ;
				}
				if (doc->version_<38) {
					Variable *cvl=instr->FindVariable(SIP_CRUSHVOL) ;
					Variable *vol=instr->FindVariable(SIP_VOLUME);
					Variable *crs=instr->FindVariable(SIP_CRUSH) ;
					if ((vol)&&(cvl)&&(crs)) {
						if (crs->GetInt()!=16) {
							int temp=vol->GetInt() ;
							vol->SetInt(cvl->GetInt()) ;
							cvl->SetInt(temp) ;
						}
					} ;
				}
			}
		}
		current=current->NextSiblingElement() ;
	} ;
} ;

void InstrumentBank::Init() {
	for (int i=0;i<MAX_INSTRUMENT_COUNT;i++) {
		instrument_[i]->Init() ;
	}
}

unsigned short InstrumentBank::GetNext() {
	for (int i=0;i<MAX_SAMPLEINSTRUMENT_COUNT;i++) {
		SampleInstrument *si=(SampleInstrument *)instrument_[i] ;
		Variable *sample=si->FindVariable(SIP_SAMPLE) ;
		if (sample) {
			if (sample->GetInt()==-1) {
				return i ;
			}
		}
	}
	return NO_MORE_INSTRUMENT ;
} ;

unsigned short InstrumentBank::Clone(unsigned short i) {
	// can't clone midi instruments

	unsigned short next=GetNext() ;
	if (next==NO_MORE_INSTRUMENT) 
  {
		return NO_MORE_INSTRUMENT ;
	}

	I_Instrument *src=instrument_[i] ;
	I_Instrument *dst=instrument_[next] ;

  if (src == dst)
  {
		return NO_MORE_INSTRUMENT ;
	}

	// Release any channels that might reference the destination instrument
	PlayerMixer *pm = PlayerMixer::GetInstance();
	if (pm) {
		pm->ReleaseInstrument(dst);
	}

	if (src->GetType()==IT_SAMPLE) {
		instrument_[next]=new SampleInstrument() ;
	} else if (src->GetType()==IT_MIDI) {
		instrument_[next]=new MidiInstrument() ;
	} else if (src->GetType()==IT_SOUNDFONT) {
		instrument_[next]=new SoundFontInstrument() ;
	} else if (src->GetType()==IT_VST3) {
		instrument_[next]=new VST3Instrument() ;
	} else {
		instrument_[next]=new LV2Instrument() ;
	}
	delete dst ;
	dst=instrument_[next] ;
	IteratorPtr<Variable> it(src->GetIterator()) ;
	for (it->Begin();!it->IsDone();it->Next()) {
		Variable &srcV=it->CurrentItem() ;
		Variable *dstV=dst->FindVariable(srcV.GetID()) ;
		if (dstV) {
			dstV->CopyFrom(srcV) ;
		}
	}
	return next ;

}

void InstrumentBank::SetInstrumentType(int i, InstrumentType type) {
    if (i < 0 || i >= MAX_INSTRUMENT_COUNT) return;
    I_Instrument *old = instrument_[i];
    if (!old) return;
    if (old->GetType() == type) return; // no change

    // create new instrument of requested type
    I_Instrument *n = nullptr;
    switch (type) {
        case IT_SAMPLE: n = new SampleInstrument(); break;
        case IT_MIDI: n = new MidiInstrument(); break;
        case IT_LV2: n = new LV2Instrument(); break;
        case IT_SOUNDFONT: n = new SoundFontInstrument(); break;
        case IT_VST3: n = new VST3Instrument(); break;
        default: return;
    }

    // copy matching variables by ID
    IteratorPtr<Variable> it(old->GetIterator());
    for (it->Begin(); !it->IsDone(); it->Next()) {
        Variable &srcV = it->CurrentItem();
        Variable *dstV = n->FindVariable(srcV.GetID());
        if (dstV) {
            dstV->CopyFrom(srcV);
        }
    }

    // Ensure the 'type' variable exists and is set appropriately
    int typeInit = 0;
    if (type == IT_LV2) typeInit = 1;
    else if (type == IT_SOUNDFONT) typeInit = 2;
    else if (type == IT_VST3) typeInit = 3;
    Variable *tv = n->FindVariable(MAKE_FOURCC('I','T','Y','P'));
    if (tv) {
        tv->SetInt(typeInit, false);
    } else {
        static char *instrTypes[] = { (char*)"Sample", (char*)"LV2", (char*)"SF2", (char*)"VST3" } ;
        WatchedVariable *ntv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 4, typeInit);
        n->Insert(ntv);
    }

    // CRITICAL: Stop any audio channels currently rendering the old
    // instrument and clear lastInstrument_ references to it. This
    // prevents the audio thread from calling Render() on freed memory.
    // ReleaseInstrument() acquires each PlayerChannel's startStopMutex_,
    // guaranteeing the audio thread is not mid-Render() when we proceed.
    PlayerMixer *pm = PlayerMixer::GetInstance();
    if (pm) {
        pm->ReleaseInstrument(old);
    }

    // Swap the pointer BEFORE deleting so any concurrent GetInstrument()
    // call sees the new (valid) object rather than a dangling pointer.
    instrument_[i] = n;
    delete old;

    // initialize new instrument
    n->Init();
}

void InstrumentBank::OnStart() {
	for (int i=0;i<MAX_INSTRUMENT_COUNT;i++) {
		instrument_[i]->OnStart() ;
	}
	init_filters() ;
} ;
