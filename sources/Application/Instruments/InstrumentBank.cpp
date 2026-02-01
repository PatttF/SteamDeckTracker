
#include "InstrumentBank.h"
#include "Application/Instruments/SampleInstrument.h"
#include "Application/Instruments/SamplePool.h"
#include "Application/Instruments/MidiInstrument.h"
#include "Application/Instruments/LV2Instrument.h"
#include "System/io/Status.h"
#include "Application/Utils/char.h"
#include "Application/Model/Config.h"
#include "Application/Persistency/PersistencyService.h"
#include "Filters.h"

char *InstrumentTypeData[IT_LAST]= {
	"Sample",
	"Midi",
	"LV2"
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
    Status::Set("All instrument loaded") ;
} ;

//
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
					}
					instrument_[id]=instr ;
				} ;

        TiXmlElement *param=current->FirstChildElement() ;
				while (param) {
					const char *name=param->Attribute("NAME") ;
					const char *value=param->Attribute("VALUE") ;

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
};

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

	delete dst ;
  
	if (src->GetType()==IT_SAMPLE) {
		dst=new SampleInstrument() ;
	} else if (src->GetType()==IT_MIDI) {
		dst=new MidiInstrument() ;
	} else {
		dst=new LV2Instrument() ;
	}
	instrument_[next]=dst ;
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

void InstrumentBank::OnStart() {
	for (int i=0;i<MAX_INSTRUMENT_COUNT;i++) {
		instrument_[i]->OnStart() ;
	}
	init_filters() ;
} ;
