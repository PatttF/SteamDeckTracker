#include "InstrumentView.h"
#include "Application/Instruments/MidiInstrument.h"
#include "Application/Instruments/SampleInstrument.h"
#include "Application/Instruments/LV2Instrument.h"
#include "Application/Instruments/SoundFontInstrument.h"
#include "Application/Instruments/VST3Instrument.h"
#include "Application/Instruments/MidiOutInstrument.h"
#include "Application/Instruments/AudioInInstrument.h"
#include "Application/Instruments/SamplePool.h"
#include "Application/Model/Config.h"
#include "Services/Midi/MidiService.h"
#include "BaseClasses/UIBigHexVarField.h"
#include "BaseClasses/UIIntVarOffField.h"
#include "BaseClasses/UINoteVarField.h"
#include "BaseClasses/UIStaticField.h"
#include "BaseClasses/UILV2ParameterField.h"
#include "BaseClasses/UIActionField.h"
#include "Foundation/Variables/Variable.h"
#include "Foundation/Variables/WatchedVariable.h"
#include "ModalDialogs/ImportSampleDialog.h"
#include "ModalDialogs/ImportLV2Dialog.h"
#include "ModalDialogs/ImportSoundFontDialog.h"
#include "ModalDialogs/ImportVST3Dialog.h"
#include "BaseClasses/UIVST3ParameterField.h"
#include "ModalDialogs/MessageBox.h"
#include "ModalDialogs/SavePresetDialog.h"
#include "ModalDialogs/RecordSampleDialog.h"
#include "System/System/System.h"
#include "System/Console/Trace.h"
#include "Application/Player/Player.h"
#include <map>
#include <vector>
#include <string>

#define ACTION_LOAD_LV2 MAKE_FOURCC('L','V','2','L')
#define ACTION_LOAD_SF2 MAKE_FOURCC('S','F','2','L')
#define ACTION_LOAD_VST3 MAKE_FOURCC('V','3','L','D')
#define ACTION_SAVE_PRESET MAKE_FOURCC('P','S','A','V')
#define ACTION_EDIT_SAMPLE MAKE_FOURCC('S','E','D','T')
#define ACTION_REC_SAMPLE MAKE_FOURCC('R','E','C','S')

// Callback for LV2 plugin selection dialog
static void LV2PluginSelectCallback(View &v, ModalView &dialog) {
	InstrumentView &iv = (InstrumentView &)v;
	iv.OnLV2PluginSelected();
}

// Callback for SF2 selection dialog
static void SF2SelectCallback(View &v, ModalView &dialog) {
	InstrumentView &iv = (InstrumentView &)v;
	iv.OnSF2Selected();
}

// Callback for VST3 plugin selection dialog
static void VST3PluginSelectCallback(View &v, ModalView &dialog) {
	InstrumentView &iv = (InstrumentView &)v;
	iv.OnVST3PluginSelected();
}

// Callback for save preset dialog
static void SavePresetCallback(View &v, ModalView &dialog) {
	SavePresetDialog &spd = (SavePresetDialog &)dialog;
	std::string name = spd.GetName();
	if (name.empty()) return;

	InstrumentView &iv = (InstrumentView &)v;
	iv.OnSavePreset(name);
}

// Callback for record sample dialog
static void RecordSampleCallback(View &v, ModalView &dialog) {
	RecordSampleDialog &rsd = (RecordSampleDialog &)dialog;
	if (rsd.GetReturnCode() == 1) {
		std::string path = rsd.GetSavedPath();
		if (!path.empty()) {
			
			// Auto-import the recorded sample into the current instrument
			Path p(path.c_str());
			SamplePool *pool = SamplePool::GetInstance();
			int sampleID = pool->ImportSample(p);
			if (sampleID >= 0) {
				int i = v.viewData_->currentInstrument_;
				InstrumentBank *bank = v.viewData_->project_->GetInstrumentBank();
				I_Instrument *instr = bank->GetInstrument(i);
				if (instr && instr->GetType() == IT_SAMPLE) {
					SampleInstrument *sinstr = (SampleInstrument *)instr;
					sinstr->AssignSample(sampleID);
					
				}
			} else {
				Trace::Error("RecordSample: failed to import %s", path.c_str());
			}
		}
	}
}

InstrumentView::InstrumentView(GUIWindow &w,ViewData *data):FieldView(w,data) {

	project_=data->project_ ;
	lastFocusID_=0 ;
	current_=0 ;
	lv2ScrollOffset_=0 ;
	pendingTypeInstrumentIdx_ = -1;
	pendingType_ = IT_SAMPLE;
	lv2LoadField_ = nullptr;
	lv2SaveField_ = nullptr;
	sf2LoadField_ = nullptr;
	vst3LoadField_ = nullptr;
	vst3SaveField_ = nullptr;
	vst3ScrollOffset_ = 0;
	// Initialize lastType_ to mirror instrument types
	for (int i = 0; i < MAX_INSTRUMENT_COUNT; ++i) {
		I_Instrument* instr = data->project_->GetInstrumentBank()->GetInstrument(i);
		if (instr && instr->FindVariable(MAKE_FOURCC('I','T','Y','P')))
			lastType_[i] = instr->FindVariable(MAKE_FOURCC('I','T','Y','P'))->GetInt();
		else {
			if (instr && instr->GetType() == IT_SOUNDFONT) lastType_[i] = 1;
			else if (instr && instr->GetType() == IT_VST3) lastType_[i] = 2;
			else if (instr && instr->GetType() == IT_LV2) lastType_[i] = 3;
			else if (instr && instr->GetType() == IT_MIDIOUT) lastType_[i] = 4;
			else if (instr && instr->GetType() == IT_AUDIOIN) lastType_[i] = 5;
			else lastType_[i] = 0;
		}
	}
	// Delay onInstrumentChange to avoid notifications during construction

}

InstrumentView::~InstrumentView() {
}

InstrumentType InstrumentView::getInstrumentType() {
	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instrument=bank->GetInstrument(i) ;
    return instrument->GetType() ;
} ;

void InstrumentView::onInstrumentChange() {

	ClearFocus() ;

	I_Instrument *old=current_ ;

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	current_=bank->GetInstrument(i) ;

	// Always clean up WatchedVariable observers before rebuilding fields,
	// whether switching instruments or just refreshing the same one.
	// This prevents stale observers from re-triggering onInstrumentChange().
	if (old!=0) {
		Variable *oldTv = old->FindVariable(MAKE_FOURCC('I','T','Y','P'));
		if (oldTv) {
			if (WatchedVariable *wtv = dynamic_cast<WatchedVariable*>(oldTv))
				wtv->RemoveObserver(*this);
		}
		if (old->GetType() == IT_SOUNDFONT) {
			Variable *oldPv = old->FindVariable(SFIP_PRESET);
			if (oldPv) {
				if (WatchedVariable *wpv = dynamic_cast<WatchedVariable*>(oldPv))
					wpv->RemoveObserver(*this);
			}
		}
		if (current_!=old) {
			old->RemoveObserver(*this) ;
		}
	}
	T_SimpleList<UIField>::Empty() ;
	lv2LoadField_ = nullptr;
	sf2LoadField_ = nullptr;
	vst3LoadField_ = nullptr;

	InstrumentType it=getInstrumentType() ;
	

    switch (it) {
		case IT_MIDI:
			fillMidiParameters() ;
			break ;
		case IT_SAMPLE:
			fillSampleParameters() ;
			break ;
		case IT_LV2:
			fillLV2Parameters() ;
			break ;
		case IT_SOUNDFONT:
			fillSoundFontParameters() ;
			break ;
		case IT_VST3:
			fillVST3Parameters() ;
			break ;
		case IT_MIDIOUT:
			fillMidiOutParameters() ;
			break ;
		case IT_AUDIOIN:
			fillAudioInParameters() ;
			break ;
	} ;

	SetFocus(T_SimpleList<UIField>::GetFirst()) ;
	IteratorPtr<UIField> it2(T_SimpleList<UIField>::GetIterator()) ;
	for (it2->Begin();!it2->IsDone();it2->Next()) {
        UIIntVarField *field = dynamic_cast<UIIntVarField *>(&it2->CurrentItem()) ;
        if (field && field->GetVariableID()==lastFocusID_) {
            SetFocus(field) ;
            break ;
        }
    } ;
	if (current_!=old) {
		current_->AddObserver(*this) ;
	}
} ;

int InstrumentView::getMidiInDeviceCount() {
	MidiService *svc = MidiService::GetInstance();
	if (!svc) return 0;
	int count = 0;
	IteratorPtr<MidiInDevice> it(svc->GetInIterator());
	for (it->Begin(); !it->IsDone(); it->Next()) count++;
	return count;
}

const char *InstrumentView::getMidiInDeviceName(int index) {
	MidiService *svc = MidiService::GetInstance();
	if (!svc) return "";
	int i = 0;
	IteratorPtr<MidiInDevice> it(svc->GetInIterator());
	for (it->Begin(); !it->IsDone(); it->Next(), i++) {
		if (i == index) return it->CurrentItem().GetName();
	}
	return "";
}

void InstrumentView::fillSampleParameters() {

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	SampleInstrument *instrument=(SampleInstrument *)instr  ;
	GUIPoint position=GetAnchor() ;
	position._y -= 1;  // shift up to make room for MIDI input footer

    // Local variables used to create fields
    Variable *v = nullptr;
    UIIntVarField *f1 = nullptr;
// Type selector: Sample vs SF2 vs VST3 vs LV2 vs MidiOut
    Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
    if (!tv) {
        static char *instrTypes[] = { (char*)"Sample", (char*)"SF2", (char*)"VST3", (char*)"LV2", (char*)"MidiOut", (char*)"AudioIn" } ;
        WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 6, 0);
        instrument->Insert(wtv);
        tv = wtv;
    }
    UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 5, 1, 7);
    T_SimpleList<UIField>::Insert(typeField);
    position._y += 1;

    // Ensure this view observes changes to the 'type' variable so we can react immediately
    if (WatchedVariable *wtv = dynamic_cast<WatchedVariable *>(tv)) {
        // Avoid duplicate observers
        wtv->RemoveObserver(*this);
        wtv->AddObserver(*this);
    }
    // Show current sample name as a clickable area that opens the Sample Import dialog
    v = instrument->FindVariable(SIP_SAMPLE);
    if (v) {
        int maxIdx = (v->GetListSize() > 0) ? (v->GetListSize() - 1) : 0;
        UIIntVarField *sampleField = new UIIntVarField(position, *v, "sample: %s", 0, maxIdx, 1, 1);
        T_SimpleList<UIField>::Insert(sampleField);
        position._y += 1;
    }
#ifdef FFMPEG_ENABLED
    v = instrument->FindVariable(SIP_PRINTFX);
    f1 = new UIIntVarField(position, *v, "%s", 0, 3, 1, 1);
    T_SimpleList<UIField>::Insert(f1) ;

    position._x += 7;
    v = instrument->FindVariable(SIP_IR_WET);
    f1 = new UIIntVarField(position, *v, "wet:%d%%", 0, 100, 1, 10);
    T_SimpleList<UIField>::Insert(f1) ;

    position._x += 9;

    v = instrument->FindVariable(SIP_IR_PAD);
    f1 = new UIIntVarField(position, *v, "pad:%dms", 0, 5000, 5, 100);
    T_SimpleList<UIField>::Insert(f1);
    position._x -= 16;
#endif
    position._y += 1;
    {
        GUIPoint editPos = position;
        UIActionField *editAf = new UIActionField("edit", ACTION_EDIT_SAMPLE, editPos);
        T_SimpleList<UIField>::Insert(editAf);
        editAf->AddObserver(*this);
    }
    position._x += 5;
    {
        GUIPoint recPos = position;
        UIActionField *recAf = new UIActionField("record", ACTION_REC_SAMPLE, recPos);
        T_SimpleList<UIField>::Insert(recAf);
        recAf->AddObserver(*this);
    }
    position._x -= 5;
    position._y += 1;
    v=instrument->FindVariable(SIP_VOLUME) ;
	f1=new UIIntVarField(position,*v,"volume: %d [%2.2X]",0,255,1,10) ;
	T_SimpleList<UIField>::Insert(f1) ;

    position._y+=1 ;
	v=instrument->FindVariable(SIP_PAN) ;
	f1=new UIIntVarField(position,*v,"pan: %2.2X",0,0xFE,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_ROOTNOTE) ;
	f1=new UINoteVarField(position,*v,"root note: %s",0,0x7F,1,0x0C) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_FINETUNE) ;
	f1=new UIIntVarField(position,*v,"detune: %2.2X",0,255,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

    position._y += 2;
    v=instrument->FindVariable(SIP_CRUSH);
	f1=new UIIntVarField(position,*v,"crush: %d",1,0x10,1,4) ;
	T_SimpleList<UIField>::Insert(f1) ;

    position._x += 10;
    v = instrument->FindVariable(SIP_CRUSHVOL);
    f1=new UIIntVarField(position,*v,"drive: %2.2X",0,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;
    position._x -= 10;

    position._y += 1;
	v=instrument->FindVariable(SIP_DOWNSMPL) ;
	f1=new UIIntVarField(position,*v,"downsample: %d",0,8,1,4) ;
	T_SimpleList<UIField>::Insert(f1) ;


	position._y+=2 ;
	UIStaticField *sf=new UIStaticField(position,"flt cut/res:") ;
	T_SimpleList<UIField>::Insert(sf) ;

	position._x+=13 ;
	v=instrument->FindVariable(SIP_FILTCUTOFF) ;
	f1=new UIIntVarField(position,*v,"%2.2X",0,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._x+=3 ;
	v=instrument->FindVariable(SIP_FILTRESO) ;
	f1=new UIIntVarField(position,*v,"%2.2X",0,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;
	position._x-=16 ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_FILTMIX) ;
	f1=new UIIntVarField(position,*v,"type: %2.2X",0,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_FILTMODE) ;
	f1=new UIIntVarField(position,*v,"Mode: %s",0,2,1,1) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_ATTENUATE) ;
	f1=new UIIntVarField(position,*v,"attenuate: %d [%2.2X]",1,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1 ;
	sf=new UIStaticField(position,"fb tune/mix: ") ;
	T_SimpleList<UIField>::Insert(sf) ;

	v=instrument->FindVariable(SIP_FBTUNE) ;
	position._x+=13 ;
	f1=new UIIntVarField(position,*v,"%2.2X",0,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._x+=3 ;
	v=instrument->FindVariable(SIP_FBMIX) ;
	f1=new UIIntVarField(position,*v,"%2.2X",0,0xFF,1,0X10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._x-=16 ;

	position._y+=2;
	v=instrument->FindVariable(SIP_INTERPOLATION) ;
	f1=new UIIntVarField(position,*v,"interpolation: %s",0,1,1,1) ;
	T_SimpleList<UIField>::Insert(f1) ;

    position._y+=1 ;
	v=instrument->FindVariable(SIP_LOOPMODE) ;
	f1=new UIIntVarField(position,*v,"loop mode: %s",0,SILM_LAST-1,1,1) ;
	T_SimpleList<UIField>::Insert(f1) ;
	position._y+=1 ;

	v=instrument->FindVariable(SIP_SLICES) ;
	f1=new UIIntVarField(position,*v,"slices: %2.2X",0,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	v=instrument->FindVariable(SIP_TABLEAUTO) ;
	position._y+=2 ;
	UIIntVarField *f2=new UIIntVarField(position,*v,"automation: %s",0,1,1,1) ;
	T_SimpleList<UIField>::Insert(f2) ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_TABLE) ;
	f1=new UIIntVarOffField(position,*v,"table: %2.2X",0x00,0x7F,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	{
		GUIPoint fp(17, position._y);
		Variable *fxv = instrument->FindVariable(IFXS);
		if (!fxv) {
			WatchedVariable *wfx = new WatchedVariable("fx slot", IFXS, -1);
			wfx->AddObserver(*this);
			instrument->Insert(wfx);
			fxv = wfx;
		}
		if (WatchedVariable *wtvfx = dynamic_cast<WatchedVariable *>(fxv)) {
			wtvfx->RemoveObserver(*this);
			wtvfx->AddObserver(*this);
		}
		UIIntVarOffField *ff = new UIIntVarOffField(fp, *fxv, "fx:%2.2X", 0x00, 0x0F, 1, 0x04);
		T_SimpleList<UIField>::Insert(ff);
	}

	// MIDI input footer row
	{
		GUIPoint p(1, position._y + 1);
		Variable *mv = instrument->FindVariable(IMDI);
		if (!mv) {
			mv = new Variable("midi dev", IMDI, 0);
			instrument->Insert(mv);
		}
		int maxDev = getMidiInDeviceCount() - 1;
		if (maxDev < 0) maxDev = 0;
		UIIntVarField *mf1 = new UIIntVarField(p, *mv, "midi:%d", 0, maxDev, 1, 1);
		T_SimpleList<UIField>::Insert(mf1);

		p._x = 14;
		mv = instrument->FindVariable(IMIC);
		if (!mv) {
			mv = new Variable("midi ch", IMIC, 0);
			instrument->Insert(mv);
		}
		UIIntVarField *mf2 = new UIIntVarField(p, *mv, "ch:%2.2d", 0, 0x0F, 1, 0x04, 1);
		T_SimpleList<UIField>::Insert(mf2);
	}

} ;

void InstrumentView::fillMidiParameters() {

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	MidiInstrument *instrument=(MidiInstrument *)instr  ;
	GUIPoint position=GetAnchor() ;

	Variable *v=instrument->FindVariable(MIP_CHANNEL) ;
	UIIntVarField* f1=new UIIntVarField(position,*v,"channel: %2.2d",0,0x0F,1,0x04,1) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1;
	v=instrument->FindVariable(MIP_VOLUME) ;
	f1=new UIIntVarField(position,*v,"volume: %2.2X",0,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1;
	v=instrument->FindVariable(MIP_NOTELENGTH) ;
	f1=new UIIntVarField(position,*v,"length: %2.2X",0,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	v=instrument->FindVariable(MIP_TABLEAUTO) ;
	position._y+=2 ;
	UIIntVarField *f2=new UIIntVarField(position,*v,"automation: %s",0,1,1,1) ;
	T_SimpleList<UIField>::Insert(f2) ;

	position._y+=1;
	v=instrument->FindVariable(MIP_TABLE) ;
	f1=new UIIntVarOffField(position,*v,"table: %2.2X",0,0x7F,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

} ;

void InstrumentView::fillMidiOutParameters() {

	// Re-scan MIDI ports for hot-plugged devices (only when not playing
	// to avoid race conditions with the audio thread)
	Player *player = Player::GetInstance();
	if (!player || !player->IsRunning()) {
		MidiService::GetInstance()->RefreshDevices();
	}

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	MidiOutInstrument *instrument=(MidiOutInstrument *)instr ;
	GUIPoint position=GetAnchor() ;
	position._y -= 1;  // shift up to make room for MIDI input footer

	// Type selector: Sample vs SF2 vs VST3 vs LV2 vs MidiOut
	Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
	if (!tv) {
		static char *instrTypes[] = { (char*)"Sample", (char*)"SF2", (char*)"VST3", (char*)"LV2", (char*)"MidiOut", (char*)"AudioIn" } ;
		WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 6, 4);
		instrument->Insert(wtv);
		tv = wtv;
	}
	UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 5, 1, 7);
	T_SimpleList<UIField>::Insert(typeField);
	position._y += 1;

	// Observe type variable for changes
	if (WatchedVariable *wtv = dynamic_cast<WatchedVariable *>(tv)) {
		wtv->RemoveObserver(*this);
		wtv->AddObserver(*this);
	}

	// Device picker
	Variable *vd = instrument->FindVariable(MOIP_DEVICE) ;
	if (vd) {
		int maxDev = instrument->GetDeviceCount() - 1;
		if (maxDev < 0) maxDev = 0;
		UIIntVarField *f1 = new UIIntVarField(position, *vd, "device: %d", 0, maxDev, 1, 1) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Channel picker (0-15)
	Variable *vc = instrument->FindVariable(MOIP_CHANNEL) ;
	if (vc) {
		UIIntVarField *f1 = new UIIntVarField(position, *vc, "channel: %2.2d", 0, 0x0F, 1, 0x04, 1) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Note length
	Variable *vl = instrument->FindVariable(MOIP_NOTELENGTH) ;
	if (vl) {
		UIIntVarField *f1 = new UIIntVarField(position, *vl, "length: %2.2X", 0, 0xFF, 1, 0x10) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Transpose
	Variable *vtr = instrument->FindVariable(MOIP_TRANSPOSE) ;
	if (vtr) {
		UIIntVarField *f1 = new UIIntVarField(position, *vtr, "transpose: %3.2d", -48, 48, 1, 0x0C) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	position._y += 1;

	// Send MIDI clock
	Variable *vck = instrument->FindVariable(MOIP_CLOCK) ;
	if (vck) {
		UIIntVarField *f1 = new UIIntVarField(position, *vck, "send clock: %s", 0, 1, 1, 1) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Send MIDI transport
	Variable *vtp = instrument->FindVariable(MOIP_TRANSPORT) ;
	if (vtp) {
		UIIntVarField *f1 = new UIIntVarField(position, *vtp, "send transport: %s", 0, 1, 1, 1) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	position._y += 1;

	// Table automation
	Variable *vta = instrument->FindVariable(MOIP_TABLEAUTO) ;
	if (vta) {
		UIIntVarField *f1 = new UIIntVarField(position, *vta, "automation: %s", 0, 1, 1, 1) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Table
	Variable *vt = instrument->FindVariable(MOIP_TABLE) ;
	if (vt) {
		UIIntVarField *f1 = new UIIntVarOffField(position, *vt, "table: %2.2X", 0, 0x7F, 1, 0x10) ;
		T_SimpleList<UIField>::Insert(f1) ;
	}
}

void InstrumentView::fillAudioInParameters() {

	// Re-scan MIDI ports for hot-plugged devices (only when not playing)
	Player *player = Player::GetInstance();
	if (!player || !player->IsRunning()) {
		MidiService::GetInstance()->RefreshDevices();
	}

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	AudioInInstrument *instrument=(AudioInInstrument *)instr ;
	GUIPoint position=GetAnchor() ;
	position._y -= 1;  // shift up to make room for MIDI input footer

	// Ensure capture device is open and registered on the MixBus
	instrument->Activate();

	// Type selector
	Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
	if (!tv) {
		static char *instrTypes[] = { (char*)"Sample", (char*)"SF2", (char*)"VST3", (char*)"LV2", (char*)"MidiOut", (char*)"AudioIn" } ;
		WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 6, 5);
		instrument->Insert(wtv);
		tv = wtv;
	}
	UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 5, 1, 7);
	T_SimpleList<UIField>::Insert(typeField);
	position._y += 1;

	// Observe type variable for changes
	if (WatchedVariable *wtv = dynamic_cast<WatchedVariable *>(tv)) {
		wtv->RemoveObserver(*this);
		wtv->AddObserver(*this);
	}

	// Input device picker
	Variable *vid = instrument->FindVariable(AIP_INPUTDEVICE) ;
	if (vid) {
		int maxDev = instrument->GetInputDeviceCount() - 1;
		if (maxDev < 0) maxDev = 0;
		UIIntVarField *f1 = new UIIntVarField(position, *vid, "input: %d", 0, maxDev, 1, 1) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Volume
	Variable *vv = instrument->FindVariable(AIP_VOLUME) ;
	if (vv) {
		UIIntVarField *f1 = new UIIntVarField(position, *vv, "volume: %2.2X", 0, 0xFF, 1, 0x10) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Pan
	Variable *vp = instrument->FindVariable(AIP_PAN) ;
	if (vp) {
		UIIntVarField *f1 = new UIIntVarField(position, *vp, "pan: %2.2X", 0, 0xFE, 1, 0x10) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	position._y += 1;

	// MIDI device picker
	Variable *vmd = instrument->FindVariable(AIP_MIDIDEVICE) ;
	if (vmd) {
		int maxDev = instrument->GetMidiDeviceCount() - 1;
		if (maxDev < 0) maxDev = 0;
		UIIntVarField *f1 = new UIIntVarField(position, *vmd, "midi dev: %d", 0, maxDev, 1, 1) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// MIDI channel picker (0-15)
	Variable *vmc = instrument->FindVariable(AIP_MIDICHANNEL) ;
	if (vmc) {
		UIIntVarField *f1 = new UIIntVarField(position, *vmc, "midi ch: %2.2d", 0, 0x0F, 1, 0x04, 1) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Note length
	Variable *vl = instrument->FindVariable(AIP_NOTELENGTH) ;
	if (vl) {
		UIIntVarField *f1 = new UIIntVarField(position, *vl, "length: %2.2X", 0, 0xFF, 1, 0x10) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Transpose
	Variable *vtr = instrument->FindVariable(AIP_TRANSPOSE) ;
	if (vtr) {
		UIIntVarField *f1 = new UIIntVarField(position, *vtr, "transpose: %3.2d", -48, 48, 1, 0x0C) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	position._y += 1;

	// Table automation
	Variable *vta = instrument->FindVariable(AIP_TABLEAUTO) ;
	if (vta) {
		UIIntVarField *f1 = new UIIntVarField(position, *vta, "automation: %s", 0, 1, 1, 1) ;
		T_SimpleList<UIField>::Insert(f1) ;
		position._y += 1;
	}

	// Table
	Variable *vtb = instrument->FindVariable(AIP_TABLE) ;
	if (vtb) {
		UIIntVarField *f1 = new UIIntVarOffField(position, *vtb, "table: %2.2X", 0, 0x7F, 1, 0x10) ;
		T_SimpleList<UIField>::Insert(f1) ;
	}

	// Effect slot (same row as table)
	{
		GUIPoint fp(17, position._y);
		Variable *fxv = instrument->FindVariable(IFXS);
		if (!fxv) {
			WatchedVariable *wfx = new WatchedVariable("fx slot", IFXS, -1);
			wfx->AddObserver(*this);
			instrument->Insert(wfx);
			fxv = wfx;
		}
		if (WatchedVariable *wtvfx = dynamic_cast<WatchedVariable *>(fxv)) {
			wtvfx->RemoveObserver(*this);
			wtvfx->AddObserver(*this);
		}
		UIIntVarOffField *ff = new UIIntVarOffField(fp, *fxv, "fx:%2.2X", 0x00, 0x0F, 1, 0x04);
		T_SimpleList<UIField>::Insert(ff);
	}
}

void InstrumentView::fillLV2Parameters() {

	lv2SaveField_ = nullptr;

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	LV2Instrument *instrument=(LV2Instrument *)instr  ;
	GUIPoint position=GetAnchor() ;
	position._y -= 1;  // shift up to make room for MIDI input footer
	
	// Constants for two-column layout
	const int COL_WIDTH = 27;      // Width of each column (25 + 2 space gap)
	const int MAX_ROWS = 17;       // Max rows for parameters (leaving room for header + footer)
	const int PARAMS_PER_PAGE = MAX_ROWS * 2;  // Two columns

    // Type selector: Sample vs SF2 vs VST3 vs LV2 vs MidiOut vs AudioIn (keep accessible to switch back)
    Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
    if (!tv) {
        static char *instrTypes[] = { (char*)"Sample", (char*)"SF2", (char*)"VST3", (char*)"LV2", (char*)"MidiOut", (char*)"AudioIn" } ;
        WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 6, 3);
        instrument->Insert(wtv);
        tv = wtv;
    }
    UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 5, 1, 7);
    T_SimpleList<UIField>::Insert(typeField);
    position._y += 1;

    // Ensure this view observes changes to the 'type' variable so we can react immediately
    if (WatchedVariable *wtv = dynamic_cast<WatchedVariable *>(tv)) {
        // Avoid duplicate observers
        wtv->RemoveObserver(*this);
        wtv->AddObserver(*this);
    }

    // Display help text at top-right (compute rightmost x based on visible logical width)
    const char *helpText = "L4/R4 to change pages";
    const int CHAR_PX = 8; // character pixel width
    int winPixelWidth = w_.GetRect().Width();
    int visibleCols = winPixelWidth / CHAR_PX;
    // Avoid requiring LOGICAL_COLS here (not always in scope). Use visibleCols with a sane fallback.
    int width = (visibleCols > 0) ? visibleCols : 80;
    int helpLen = (int)strlen(helpText);
    int helpX = width - helpLen - View::margin_;
    if (helpX < 0) helpX = 0;
    GUIPoint helpPos(helpX, 0);
    UIStaticField *helpField = new UIStaticField(helpPos, helpText);
    T_SimpleList<UIField>::Insert(helpField);

    // Display the plugin selector as a clickable action (opens LV2 browser)
    if (instrument->IsEmpty()) {
        strcpy(lv2PluginLabel_, "plugin: ---");
    } else {
        snprintf(lv2PluginLabel_, 80, "plugin: %s", instrument->GetName());
    }
    UIActionField *af = new UIActionField(lv2PluginLabel_, ACTION_LOAD_LV2, position);
    T_SimpleList<UIField>::Insert(af);
    af->AddObserver(*this);
    // Keep pointer so we can react to A presses explicitly
    lv2LoadField_ = af;

    position._y+=1;

	// Show bank/preset selectors if plugin has presets
	if (!instrument->IsEmpty() && instrument->GetBankCount() > 0) {
		// Bank selector (if more than one bank)
		if (instrument->GetBankCount() > 1) {
			Variable *bv = instrument->FindVariable(LV2IP_BANK);
			if (!bv) {
				WatchedVariable *wbv = new WatchedVariable("bank", LV2IP_BANK, 0);
				wbv->AddObserver(*this);
				instrument->Insert(wbv);
				bv = wbv;
			}
			if (WatchedVariable *wbv = dynamic_cast<WatchedVariable *>(bv)) {
				wbv->RemoveObserver(*this);
				wbv->AddObserver(*this);
			}
			int maxBank = instrument->GetBankCount() - 1;
			if (maxBank < 0) maxBank = 0;
			UIIntVarField *bf = new UIIntVarField(position, *bv, "bank: %2.2X", 0, maxBank, 1, 0x10);
			T_SimpleList<UIField>::Insert(bf);
			position._y += 1;

			// Show bank name
			int bankIdx = instrument->GetCurrentBank();
			const char *bankName = instrument->GetBankName(bankIdx);
			snprintf(lv2BankLabel_, sizeof(lv2BankLabel_), "  [%s]", bankName);
			UIStaticField *bsf = new UIStaticField(position, lv2BankLabel_);
			T_SimpleList<UIField>::Insert(bsf);
			position._y += 1;
		}

		// Preset selector
		Variable *pv = instrument->FindVariable(LV2IP_PRESET);
		if (!pv) {
			WatchedVariable *wpv = new WatchedVariable("preset", LV2IP_PRESET, 0);
			wpv->AddObserver(*this);
			instrument->Insert(wpv);
			pv = wpv;
		}
		if (WatchedVariable *wpv = dynamic_cast<WatchedVariable *>(pv)) {
			wpv->RemoveObserver(*this);
			wpv->AddObserver(*this);
		}
		// Sync variable to actual preset
		if (pv->GetInt() != instrument->GetCurrentPreset()) {
			pv->SetInt(instrument->GetCurrentPreset());
		}
		int maxPreset = instrument->GetPresetCount() - 1;
		if (maxPreset < 0) maxPreset = 0;
		UIIntVarField *pf = new UIIntVarField(position, *pv, "preset: %2.2X", 0, maxPreset, 1, 0x10);
		T_SimpleList<UIField>::Insert(pf);
		position._y += 1;

		// Show preset name
		int presetIdx = instrument->GetCurrentPreset();
		const char *presetName = instrument->GetPresetName(presetIdx);
		snprintf(lv2PresetLabel_, sizeof(lv2PresetLabel_), "  [%s]", presetName);
		UIStaticField *sf = new UIStaticField(position, lv2PresetLabel_);
		T_SimpleList<UIField>::Insert(sf);
		position._y += 1;

		// Show save preset action if supported
		if (instrument->canSavePreset()) {
			UIActionField *savAf = new UIActionField("save preset", ACTION_SAVE_PRESET, position);
			T_SimpleList<UIField>::Insert(savAf);
			savAf->AddObserver(*this);
			lv2SaveField_ = savAf;
			position._y += 1;
		}
	}

	// Show parameters if plugin is loaded
	if (!instrument->IsEmpty()) {
		int paramCount = instrument->GetParameterCount();
		if (paramCount > 0) {
			// Group parameters by groupName, preserving order
			std::vector<std::pair<std::string, std::vector<int>>> groups;
			std::map<std::string, int> groupIndex;
			
			for (int p = 0; p < paramCount; p++) {
				const LV2PluginParameter *param = instrument->GetParameter(p);
				if (!param) continue;
				
				std::string grp = param->groupName.empty() ? "" : param->groupName;
				if (groupIndex.find(grp) == groupIndex.end()) {
					groupIndex[grp] = (int)groups.size();
					groups.push_back({grp, {}});
				}
				groups[groupIndex[grp]].second.push_back(p);
			}
			
			// Flatten grouped params into display order
			std::vector<int> displayOrder;
			for (auto &grp : groups) {
				for (int idx : grp.second) {
					displayOrder.push_back(idx);
				}
			}
			
			// Calculate scroll range
			int totalParams = (int)displayOrder.size();
			int startIdx = lv2ScrollOffset_;
			if (startIdx >= totalParams) startIdx = 0;
			int endIdx = startIdx + PARAMS_PER_PAGE;
			if (endIdx > totalParams) endIdx = totalParams;
			
			// Display in two columns - left-align for long labels
			int baseY = position._y;
			int baseX = 1;
			int col = 0;
			int row = 0;
			int textIdx = 0;
			
			for (int i = startIdx; i < endIdx && textIdx < 40; i++, textIdx++) {
				int p = displayOrder[i];
				const LV2PluginParameter *param = instrument->GetParameter(p);
				if (!param || !param->variable) continue;
				
				// Position for this parameter - columns at left edge
				GUIPoint pos(baseX + col * COL_WIDTH, baseY + row);
				
				// Truncate name to fit column (allow more chars now that we're left-aligned)
				strncpy(lv2ParamText_[textIdx], param->name.c_str(), 22);
				lv2ParamText_[textIdx][22] = '\0';
				
				// Calculate step size: 127 steps should span the actual min/max range
				// So each +1 on 0-127 = (max-min)/127 actual value change
				int stepSize = 1;
				int bigStep = 10;
				
				// For enumerated params with scale points, step to next point
				if (!param->scalePoints.empty()) {
					int numPoints = (int)param->scalePoints.size();
					if (numPoints > 1) {
						// Step size to move between scale points
						stepSize = 127 / (numPoints - 1);
						if (stepSize < 1) stepSize = 1;
						bigStep = stepSize * 4;
					}
				}
				
				// Variable stores values scaled to 0-127, so use those as min/max
				UILV2ParameterField *pfield = new UILV2ParameterField(
					pos, 
					*param->variable, 
					lv2ParamText_[textIdx],
					instrument,
					p,
					0,    // min: Variable is scaled 0-127
					127,  // max: Variable is scaled 0-127
					stepSize, 
					bigStep
				);
				T_SimpleList<UIField>::Insert(pfield);

                // Diagnostic: log whether this variable is a WatchedVariable and its id
                WatchedVariable *wv_check = dynamic_cast<WatchedVariable*>(param->variable);
                

                // Advance row / column cursor for two-column layout
                row++;
                if (row >= MAX_ROWS) {
                    row = 0;
                    col++;
                    if (col >= 2) break;  // Only two columns
                }
            }

            // Move position down past the parameter grid
                position._y = baseY + MAX_ROWS + 1;
            }
        } else {
            // No plugin loaded: leave a small gap
            position._y += 2;
        }

    // Footer controls — single horizontal row
	{
		GUIPoint p(1, position._y);
		Variable *v = instrument->FindVariable(LV2IP_VOLUME);
		UIIntVarField *f1 = new UIIntVarField(p, *v, "vol:%2.2X", 0, 0xFF, 1, 0x10);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 9;
		v = instrument->FindVariable(LV2IP_PAN);
		f1 = new UIIntVarField(p, *v, "pan:%2.2X", 0, 0xFE, 1, 0x10);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 17;
		v = instrument->FindVariable(LV2IP_TABLEAUTO);
		UIIntVarField *f2 = new UIIntVarField(p, *v, "auto:%s", 0, 1, 1, 1);
		T_SimpleList<UIField>::Insert(f2);

		p._x = 29;
		v = instrument->FindVariable(LV2IP_TABLE);
		f1 = new UIIntVarOffField(p, *v, "tbl:%2.2X", 0, 0x7F, 1, 0x10);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 37;
		v = instrument->FindVariable(IFXS);
		if (!v) {
			WatchedVariable *wfx = new WatchedVariable("fx slot", IFXS, -1);
			wfx->AddObserver(*this);
			instrument->Insert(wfx);
			v = wfx;
		}
		if (WatchedVariable *wtvfx = dynamic_cast<WatchedVariable *>(v)) {
			wtvfx->RemoveObserver(*this);
			wtvfx->AddObserver(*this);
		}
		UIIntVarOffField *f3 = new UIIntVarOffField(p, *v, "fx:%2.2X", 0x00, 0x0F, 1, 0x04);
		T_SimpleList<UIField>::Insert(f3);
	}

	// MIDI input footer row
	{
		GUIPoint p(1, position._y + 1);
		Variable *v = instrument->FindVariable(IMDI);
		if (!v) {
			v = new Variable("midi dev", IMDI, 0);
			instrument->Insert(v);
		}
		int maxDev = getMidiInDeviceCount() - 1;
		if (maxDev < 0) maxDev = 0;
		UIIntVarField *f1 = new UIIntVarField(p, *v, "midi:%d", 0, maxDev, 1, 1);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 14;
		v = instrument->FindVariable(IMIC);
		if (!v) {
			v = new Variable("midi ch", IMIC, 0);
			instrument->Insert(v);
		}
		UIIntVarField *f2 = new UIIntVarField(p, *v, "ch:%2.2d", 0, 0x0F, 1, 0x04, 1);
		T_SimpleList<UIField>::Insert(f2);
	}

} ;

void InstrumentView::fillSoundFontParameters() {

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	SoundFontInstrument *instrument=(SoundFontInstrument *)instr  ;
	GUIPoint position=GetAnchor() ;
	position._y -= 1;  // shift up to make room for MIDI input footer

	// Local variables used to create fields
	Variable *v = nullptr;
	UIIntVarField *f1 = nullptr;

	// Type selector: Sample vs SF2 vs VST3 vs LV2 vs MidiOut vs AudioIn
	Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
	if (!tv) {
		static char *instrTypes[] = { (char*)"Sample", (char*)"SF2", (char*)"VST3", (char*)"LV2", (char*)"MidiOut", (char*)"AudioIn" } ;
		WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 6, 1);
		instrument->Insert(wtv);
		tv = wtv;
	}
	UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 5, 1, 7);
	T_SimpleList<UIField>::Insert(typeField);
	position._y += 1;

	// Ensure this view observes changes to the 'type' variable
	if (WatchedVariable *wtv = dynamic_cast<WatchedVariable *>(tv)) {
		wtv->RemoveObserver(*this);
		wtv->AddObserver(*this);
	}

	// Display the SF2 loader as a clickable action field
	if (instrument->IsEmpty()) {
		strcpy(sf2Label_, "soundfont: ---");
	} else {
		snprintf(sf2Label_, sizeof(sf2Label_), "soundfont: %s", instrument->GetName());
	}
	UIActionField *af = new UIActionField(sf2Label_, ACTION_LOAD_SF2, position);
	T_SimpleList<UIField>::Insert(af);
	af->AddObserver(*this);
	sf2LoadField_ = af;

	position._y += 1;

	// Show preset selector (interactive)
	if (!instrument->IsEmpty() && instrument->GetPresetCount() > 0) {
		Variable *pv = instrument->FindVariable(SFIP_PRESET);
		if (pv) {
			// Observe changes to the preset variable
			if (WatchedVariable *wpv = dynamic_cast<WatchedVariable *>(pv)) {
				wpv->RemoveObserver(*this);
				wpv->AddObserver(*this);
			}
			// Sync variable to actual preset in case it's out of date
			if (pv->GetInt() != instrument->GetCurrentPreset()) {
				pv->SetInt(instrument->GetCurrentPreset());
			}
			int maxPreset = instrument->GetPresetCount() - 1;
			if (maxPreset < 0) maxPreset = 0;
			UIIntVarField *pf = new UIIntVarField(position, *pv, "preset: %2.2X", 0, maxPreset, 1, 0x10);
			T_SimpleList<UIField>::Insert(pf);
			position._y += 1;

			// Show preset name as static label below
			int presetIdx = instrument->GetCurrentPreset();
			const char *presetName = instrument->GetPresetName(presetIdx);
			snprintf(sf2PresetLabel_, sizeof(sf2PresetLabel_), "  [%s]", presetName);
			UIStaticField *sf = new UIStaticField(position, sf2PresetLabel_);
			T_SimpleList<UIField>::Insert(sf);
			position._y += 1;
		}
	}

	position._y += 1;

	// Footer controls — single horizontal row
	{
		GUIPoint p(1, position._y);
		v = instrument->FindVariable(SFIP_VOLUME);
		f1 = new UIIntVarField(p, *v, "vol:%2.2X", 0, 0xFF, 1, 0x10);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 9;
		v = instrument->FindVariable(SFIP_PAN);
		f1 = new UIIntVarField(p, *v, "pan:%2.2X", 0, 0xFE, 1, 0x10);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 17;
		v = instrument->FindVariable(SFIP_TABLEAUTO);
		UIIntVarField *f2 = new UIIntVarField(p, *v, "auto:%s", 0, 1, 1, 1);
		T_SimpleList<UIField>::Insert(f2);

		p._x = 29;
		v = instrument->FindVariable(SFIP_TABLE);
		f1 = new UIIntVarOffField(p, *v, "tbl:%2.2X", 0x00, 0x7F, 1, 0x10);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 37;
		v = instrument->FindVariable(IFXS);
		if (!v) {
			WatchedVariable *wfx = new WatchedVariable("fx slot", IFXS, -1);
			wfx->AddObserver(*this);
			instrument->Insert(wfx);
			v = wfx;
		}
		if (WatchedVariable *wtvfx = dynamic_cast<WatchedVariable *>(v)) {
			wtvfx->RemoveObserver(*this);
			wtvfx->AddObserver(*this);
		}
		UIIntVarOffField *f3 = new UIIntVarOffField(p, *v, "fx:%2.2X", 0x00, 0x0F, 1, 0x04);
		T_SimpleList<UIField>::Insert(f3);
	}

	// MIDI input footer row
	{
		GUIPoint p(1, position._y + 1);
		Variable *v = instrument->FindVariable(IMDI);
		if (!v) {
			v = new Variable("midi dev", IMDI, 0);
			instrument->Insert(v);
		}
		int maxDev = getMidiInDeviceCount() - 1;
		if (maxDev < 0) maxDev = 0;
		UIIntVarField *f1 = new UIIntVarField(p, *v, "midi:%d", 0, maxDev, 1, 1);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 14;
		v = instrument->FindVariable(IMIC);
		if (!v) {
			v = new Variable("midi ch", IMIC, 0);
			instrument->Insert(v);
		}
		UIIntVarField *f2 = new UIIntVarField(p, *v, "ch:%2.2d", 0, 0x0F, 1, 0x04, 1);
		T_SimpleList<UIField>::Insert(f2);
	}
}

void InstrumentView::fillVST3Parameters() {

	vst3SaveField_ = nullptr;

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	VST3Instrument *instrument=(VST3Instrument *)instr ;
	GUIPoint position=GetAnchor() ;
	position._y -= 1;  // shift up to make room for MIDI input footer

	const int COL_WIDTH = 27;
	const int MAX_ROWS = 17;
	const int PARAMS_PER_PAGE = MAX_ROWS * 2;

	// Type selector
	Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
	if (!tv) {
		static char *instrTypes[] = { (char*)"Sample", (char*)"SF2", (char*)"VST3", (char*)"LV2", (char*)"MidiOut", (char*)"AudioIn" } ;
		WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 6, 2);
		instrument->Insert(wtv);
		tv = wtv;
	}
	UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 5, 1, 7);
	T_SimpleList<UIField>::Insert(typeField);
	position._y += 1;

	if (WatchedVariable *wtv = dynamic_cast<WatchedVariable *>(tv)) {
		wtv->RemoveObserver(*this);
		wtv->AddObserver(*this);
	}

	// Page scroll help
	const char *helpText = "L4/R4 to change pages";
	const int CHAR_PX = 8;
	int winPixelWidth = w_.GetRect().Width();
	int visibleCols = winPixelWidth / CHAR_PX;
	int width = (visibleCols > 0) ? visibleCols : 80;
	int helpLen = (int)strlen(helpText);
	int helpX = width - helpLen - View::margin_;
	if (helpX < 0) helpX = 0;
	GUIPoint helpPos(helpX, 0);
	UIStaticField *helpField = new UIStaticField(helpPos, helpText);
	T_SimpleList<UIField>::Insert(helpField);

	// Plugin selector
	if (instrument->IsEmpty()) {
		strcpy(vst3PluginLabel_, "plugin: ---");
	} else {
		snprintf(vst3PluginLabel_, 80, "plugin: %s", instrument->GetName());
	}
	UIActionField *af = new UIActionField(vst3PluginLabel_, ACTION_LOAD_VST3, position);
	T_SimpleList<UIField>::Insert(af);
	af->AddObserver(*this);
	vst3LoadField_ = af;

	position._y += 1;

	// Show bank/preset selectors if plugin has presets
	if (!instrument->IsEmpty() && instrument->GetBankCount() > 0) {
		// Bank selector (if more than one bank)
		if (instrument->GetBankCount() > 1) {
			Variable *bv = instrument->FindVariable(VST3IP_BANK);
			if (!bv) {
				int maxBank = instrument->GetBankCount() - 1;
				WatchedVariable *wbv = new WatchedVariable("bank", VST3IP_BANK, 0);
				wbv->AddObserver(*this);
				instrument->Insert(wbv);
				bv = wbv;
			}
			if (WatchedVariable *wbv = dynamic_cast<WatchedVariable *>(bv)) {
				wbv->RemoveObserver(*this);
				wbv->AddObserver(*this);
			}
			int maxBank = instrument->GetBankCount() - 1;
			if (maxBank < 0) maxBank = 0;
			UIIntVarField *bf = new UIIntVarField(position, *bv, "bank: %2.2X", 0, maxBank, 1, 0x10);
			T_SimpleList<UIField>::Insert(bf);
			position._y += 1;

			// Show bank name
			int bankIdx = instrument->GetCurrentBank();
			const char *bankName = instrument->GetBankName(bankIdx);
			snprintf(vst3BankLabel_, sizeof(vst3BankLabel_), "  [%s]", bankName);
			UIStaticField *bsf = new UIStaticField(position, vst3BankLabel_);
			T_SimpleList<UIField>::Insert(bsf);
			position._y += 1;
		}

		// Preset selector
		Variable *pv = instrument->FindVariable(VST3IP_PRESET);
		if (!pv) {
			WatchedVariable *wpv = new WatchedVariable("preset", VST3IP_PRESET, 0);
			wpv->AddObserver(*this);
			instrument->Insert(wpv);
			pv = wpv;
		}
		if (WatchedVariable *wpv = dynamic_cast<WatchedVariable *>(pv)) {
			wpv->RemoveObserver(*this);
			wpv->AddObserver(*this);
		}
		// Sync variable to actual preset
		if (pv->GetInt() != instrument->GetCurrentPreset()) {
			pv->SetInt(instrument->GetCurrentPreset());
		}
		int maxPreset = instrument->GetPresetCount() - 1;
		if (maxPreset < 0) maxPreset = 0;
		UIIntVarField *pf = new UIIntVarField(position, *pv, "preset: %2.2X", 0, maxPreset, 1, 0x10);
		T_SimpleList<UIField>::Insert(pf);
		position._y += 1;

		// Show preset name
		int presetIdx = instrument->GetCurrentPreset();
		const char *presetName = instrument->GetPresetName(presetIdx);
		snprintf(vst3PresetLabel_, sizeof(vst3PresetLabel_), "  [%s]", presetName);
		UIStaticField *sf = new UIStaticField(position, vst3PresetLabel_);
		T_SimpleList<UIField>::Insert(sf);
		position._y += 1;

		// Show save preset action if supported
		if (instrument->canSavePreset()) {
			UIActionField *savAf = new UIActionField("save preset", ACTION_SAVE_PRESET, position);
			T_SimpleList<UIField>::Insert(savAf);
			savAf->AddObserver(*this);
			vst3SaveField_ = savAf;
			position._y += 1;
		}
	}

	// Show parameters if plugin is loaded
	if (!instrument->IsEmpty()) {
		int paramCount = instrument->GetParameterCount();
		if (paramCount > 0) {
			int totalParams = paramCount;
			int startIdx = vst3ScrollOffset_;
			if (startIdx >= totalParams) startIdx = 0;
			int endIdx = startIdx + PARAMS_PER_PAGE;
			if (endIdx > totalParams) endIdx = totalParams;

			int baseY = position._y;
			int baseX = 1;
			int col = 0;
			int row = 0;
			int textIdx = 0;

			for (int p = startIdx; p < endIdx && textIdx < 40; p++, textIdx++) {
				const VST3PluginParameter *param = instrument->GetParameter(p);
				if (!param || !param->variable) continue;

				GUIPoint pos(baseX + col * COL_WIDTH, baseY + row);

				strncpy(vst3ParamText_[textIdx], param->name.c_str(), 22);
				vst3ParamText_[textIdx][22] = '\0';

				int stepSize = 1;
				int bigStep = 10;
				int maxVal = (param->stepCount > 0 && param->stepCount <= 255) ? param->stepCount : 255;

				UIVST3ParameterField *pfield = new UIVST3ParameterField(
					pos,
					*param->variable,
					vst3ParamText_[textIdx],
					instrument,
					p,
					0,
					maxVal,
					stepSize,
					bigStep
				);
				T_SimpleList<UIField>::Insert(pfield);

				row++;
				if (row >= MAX_ROWS) {
					row = 0;
					col++;
					if (col >= 2) break;
				}
			}

			position._y = baseY + MAX_ROWS + 1;
		}
	} else {
		position._y += 2;
	}

	// Footer controls — single horizontal row
	{
		GUIPoint p(1, position._y);
		Variable *v = instrument->FindVariable(VST3IP_VOLUME);
		UIIntVarField *f1 = new UIIntVarField(p, *v, "vol:%2.2X", 0, 0xFF, 1, 0x10);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 9;
		v = instrument->FindVariable(VST3IP_PAN);
		f1 = new UIIntVarField(p, *v, "pan:%2.2X", 0, 0xFE, 1, 0x10);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 17;
		v = instrument->FindVariable(VST3IP_TABLEAUTO);
		UIIntVarField *f2 = new UIIntVarField(p, *v, "auto:%s", 0, 1, 1, 1);
		T_SimpleList<UIField>::Insert(f2);

		p._x = 29;
		v = instrument->FindVariable(VST3IP_TABLE);
		f1 = new UIIntVarOffField(p, *v, "tbl:%2.2X", 0, 0x7F, 1, 0x10);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 37;
		v = instrument->FindVariable(IFXS);
		if (!v) {
			WatchedVariable *wfx = new WatchedVariable("fx slot", IFXS, -1);
			wfx->AddObserver(*this);
			instrument->Insert(wfx);
			v = wfx;
		}
		if (WatchedVariable *wtvfx = dynamic_cast<WatchedVariable *>(v)) {
			wtvfx->RemoveObserver(*this);
			wtvfx->AddObserver(*this);
		}
		UIIntVarOffField *f3 = new UIIntVarOffField(p, *v, "fx:%2.2X", 0x00, 0x0F, 1, 0x04);
		T_SimpleList<UIField>::Insert(f3);
	}

	// MIDI input footer row
	{
		GUIPoint p(1, position._y + 1);
		Variable *v = instrument->FindVariable(IMDI);
		if (!v) {
			v = new Variable("midi dev", IMDI, 0);
			instrument->Insert(v);
		}
		int maxDev = getMidiInDeviceCount() - 1;
		if (maxDev < 0) maxDev = 0;
		UIIntVarField *f1 = new UIIntVarField(p, *v, "midi:%d", 0, maxDev, 1, 1);
		T_SimpleList<UIField>::Insert(f1);

		p._x = 14;
		v = instrument->FindVariable(IMIC);
		if (!v) {
			v = new Variable("midi ch", IMIC, 0);
			instrument->Insert(v);
		}
		UIIntVarField *f2 = new UIIntVarField(p, *v, "ch:%2.2d", 0, 0x0F, 1, 0x04, 1);
		T_SimpleList<UIField>::Insert(f2);
	}
}


void InstrumentView::warpToNext(int offset) {
	int instrument=viewData_->currentInstrument_+offset ;
	if (instrument>=MAX_INSTRUMENT_COUNT) {
		instrument=instrument-MAX_INSTRUMENT_COUNT ;
	} ;
	if (instrument<0) {
		instrument=MAX_INSTRUMENT_COUNT+instrument ;
	} ;
	viewData_->currentInstrument_=instrument ;
	onInstrumentChange() ;
	isDirty_=true ;
} ;

void InstrumentView::ProcessButtonMask(unsigned short mask,bool pressed) {

	if (!pressed) return ;

	isDirty_=false ;

	if (viewMode_==VM_NEW) {
		if (mask==EPBM_A) {
			UIField *focusField = GetFocus();
			if (focusField == lv2LoadField_) {
				InstrumentType it = getInstrumentType();
				if (it == IT_LV2) {
					ImportLV2Dialog *dialog = new ImportLV2Dialog(*this);
					DoModal(dialog, LV2PluginSelectCallback);
				}
				return;
			}
			if (focusField == lv2SaveField_) {
				InstrumentType it = getInstrumentType();
				if (it == IT_LV2) {
					SavePresetDialog *dialog = new SavePresetDialog(*this);
					DoModal(dialog, SavePresetCallback);
				}
				return;
			}
			if (focusField == sf2LoadField_) {
				InstrumentType it = getInstrumentType();
				if (it == IT_SOUNDFONT) {
					ImportSoundFontDialog *dialog = new ImportSoundFontDialog(*this);
					DoModal(dialog, SF2SelectCallback);
				}
				return;
			}
			if (focusField == vst3LoadField_) {
				InstrumentType it = getInstrumentType();
				if (it == IT_VST3) {
					ImportVST3Dialog *dialog = new ImportVST3Dialog(*this);
					DoModal(dialog, VST3PluginSelectCallback);
				}
				return;
			}
			if (focusField == vst3SaveField_) {
				InstrumentType it = getInstrumentType();
				if (it == IT_VST3) {
					SavePresetDialog *dialog = new SavePresetDialog(*this);
					DoModal(dialog, SavePresetCallback);
				}
				return;
			}
			// For variable-based fields
			if (mask&EPBM_A) {
				UIIntVarField *field=(UIIntVarField *)GetFocus() ;
				Variable &v=field->GetVariable() ;
				switch(v.GetID()) {
					case SIP_SAMPLE:
						 { 				// Prevent importing a sample if the instrument has been switched to lv2 (LV2)
					InstrumentType curType = getInstrumentType();
					if (curType != IT_SAMPLE) {
						MessageBox *mb = new MessageBox(*this, "Cannot import a sample when Type is 'LV2'", MBBF_OK);
						DoModal(mb);
						break;
					}
						// First check if the samplelib exists

						 Path sampleLib(SamplePool::GetInstance()->GetSampleLib()) ;
						 if (FileSystem::GetInstance()->GetFileType(sampleLib.GetPath().c_str())!=FT_DIR) {
							 MessageBox *mb=new MessageBox(*this,"Can't access the samplelib",MBBF_OK) ;
							 DoModal(mb) ;
						 } else { ;
							// Go to import sample

							 ImportSampleDialog *isd=new ImportSampleDialog(*this) ;
							 DoModal(isd) ;
						}
						break ;
					 }
					case SIP_TABLE:
				 {
					int next=TableHolder::GetInstance()->GetNext() ;
					if (next!=NO_MORE_TABLE) {
						v.SetInt(next) ;
						isDirty_=true ;
					}
					break ;
                }
                case SIP_PRINTFX: {
                    FxPrinter printer(viewData_);
                    isDirty_ = printer.Run();
                    View::SetNotification(printer.GetNotification());
                    break;
                }
                default:
                    break ;
				}
			}
		}
		// Only remove A/L when it was a pure A press — keep A for A+arrow so fields receive it
		if (mask==EPBM_A) {
			mask&=(0xFFFF-(EPBM_A|EPBM_L)) ;
		}
	} ;

	if (viewMode_==VM_SELECTION) {
	} else {
		viewMode_=VM_NORMAL ;
	}


	FieldView::ProcessButtonMask(mask) ;

    Player *player=Player::GetInstance() ;
    
	// B single-press no longer opens the LV2 list. Use A on 'load list' to open the LV2 browser.
	// (Previous behavior removed)

	
	// B Modifier

    if (mask & EPBM_B) {
        if (mask&EPBM_LEFT) warpToNext(-1) ;
		if (mask&EPBM_RIGHT) warpToNext(+1);
		if (mask&EPBM_DOWN) warpToNext(-16) ;
		if (mask&EPBM_UP) warpToNext(+16);
		if (mask&EPBM_A) { // Allow cut instrument
		   if (getInstrumentType()==IT_SAMPLE) {
                if (GetFocus()==T_SimpleList<UIField>::GetFirst()) {
	               int i=viewData_->currentInstrument_ ;
	               InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	               I_Instrument *instr=bank->GetInstrument(i) ;
					instr->Purge() ;
//                   Variable *v=instr->FindVariable(SIP_SAMPLE) ;
//                   v->SetInt(-1) ;
                   isDirty_=true ;
                }
           }

		   // Check if on table
		   if (GetFocus()==T_SimpleList<UIField>::GetLast()) {
	            int i=viewData_->currentInstrument_ ;
	            InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	            I_Instrument *instr=bank->GetInstrument(i) ;
                Variable *v=instr->FindVariable(SIP_TABLE) ;
                if (v) {
                    v->SetInt(-1) ;
                    isDirty_=true ;
                }
		   } ;
        }
        if (mask&EPBM_L) {
            viewMode_=VM_CLONE ;
        } ;
    } else {

        // A modifier

        if (mask == EPBM_A) {
                UIIntVarField *focusField = dynamic_cast<UIIntVarField *>((UIIntVarField *)GetFocus());
                if (focusField) {
                    FourCC varID = focusField->GetVariableID();
                    // Single-press on sample opens the sample browser immediately
                    if (varID == SIP_SAMPLE) {
                        InstrumentType curType = getInstrumentType();
                        if (curType != IT_SAMPLE) {
                            MessageBox *mb = new MessageBox(*this, "Cannot import a sample when Type is 'LV2'", MBBF_OK);
                            DoModal(mb);
                        } else {
                            Path sampleLib(SamplePool::GetInstance()->GetSampleLib());
                            if (FileSystem::GetInstance()->GetFileType(sampleLib.GetPath().c_str()) != FT_DIR) {
                                MessageBox *mb = new MessageBox(*this, "Can't access the samplelib", MBBF_OK);
                                DoModal(mb);
                            } else {
                                ImportSampleDialog *isd = new ImportSampleDialog(*this);
                                DoModal(isd);
                            }
                        }
                        return;
                    }

                    if ((varID == SIP_TABLE) || (varID == MIP_TABLE) ||
                        (varID == SIP_SAMPLE) || (varID == SIP_PRINTFX)) {
                        viewMode_ = VM_NEW;
                    }
                }
            } else {

            // R Modifier

            if (mask & EPBM_R) {
                if (mask & EPBM_LEFT) {
                    ViewType vt = VT_PHRASE;
                    ViewEvent ve(VET_SWITCH_VIEW, &vt);
                    SetChanged();
                    NotifyObservers(&ve);
                }

                if (mask & EPBM_DOWN) {

                    // Go to table view

                    ViewType vt = VT_TABLE2;

                    int i = viewData_->currentInstrument_;
                    InstrumentBank *bank =
                        viewData_->project_->GetInstrumentBank();
                    I_Instrument *instr = bank->GetInstrument(i);
                    int table = instr->GetTable();
                    if (table != VAR_OFF) {
                        viewData_->currentTable_ = table;
                    }
                    ViewEvent ve(VET_SWITCH_VIEW, &vt);
                    SetChanged();
                    NotifyObservers(&ve);
                }

                // if (mask&EPBM_RIGHT) {

                //	// Go to import sample

                //		ViewType vt=VT_IMPORT ;
                //		ViewEvent ve(VET_SWITCH_VIEW,&vt) ;
                //		SetChanged();
                //		NotifyObservers(&ve) ;
                //}

                if (mask & EPBM_RIGHT) {
                    // Go to effect view
                    ViewType vt = VT_EFFECT;
                    ViewEvent ve(VET_SWITCH_VIEW, &vt);
                    SetChanged();
                    NotifyObservers(&ve);
                }

                if (mask & EPBM_START) {
                    player->OnStartButton(PM_PHRASE, viewData_->songX_, true,
                                          viewData_->chainRow_);
                }
            } else if (mask & (EPBM_L | EPBM_PGBACK | EPBM_PGFWD)) {
                // L / > scroll forward, < scrolls backward
                InstrumentType it=getInstrumentType() ;
                const int SCROLL_STEP = 18;
                const int PARAMS_PER_PAGE = 36;  // 18 rows x 2 columns
                int paramCount = 0;
                int *scrollOffset = nullptr;

                if (it==IT_LV2) {
                    LV2Instrument *lv2instr=(LV2Instrument *)current_ ;
                    if (!lv2instr->IsEmpty()) {
                        paramCount = lv2instr->GetParameterCount();
                        scrollOffset = &lv2ScrollOffset_;
                    }
                } else if (it==IT_VST3) {
                    VST3Instrument *vst3instr=(VST3Instrument *)current_ ;
                    if (!vst3instr->IsEmpty()) {
                        paramCount = vst3instr->GetParameterCount();
                        scrollOffset = &vst3ScrollOffset_;
                    }
                }

                if (scrollOffset && paramCount > PARAMS_PER_PAGE) {
                    if (mask & EPBM_PGBACK) {
                        // Scroll backward
                        *scrollOffset -= SCROLL_STEP;
                        if (*scrollOffset < 0) {
                            *scrollOffset = ((paramCount - 1) / SCROLL_STEP) * SCROLL_STEP;
                        }
                    } else {
                        // L or > scroll forward
                        *scrollOffset += SCROLL_STEP;
                        if (*scrollOffset >= paramCount) {
                            *scrollOffset = 0;
                        }
                    }
                    onInstrumentChange();
                    isDirty_=true;
                    return;
                }
            } else {
                // No modifier
                if (mask & EPBM_START) {
                    player->OnStartButton(PM_PHRASE, viewData_->songX_, false,
                                          viewData_->chainRow_);
                }
            }
        }
    }

    UIIntVarField *field = dynamic_cast<UIIntVarField *>(GetFocus());
    if (field) {
        lastFocusID_=field->GetVariableID() ;
    }
}


void InstrumentView::drawAudioInHelp() {
	InstrumentType it = getInstrumentType();
	if (it != IT_AUDIOIN) return;

	UIField *f = GetFocus();
	if (!f) return;

	const char *helpLine1 = "";
	const char *helpLine2 = "";

	UIIntVarField *vf = dynamic_cast<UIIntVarField *>(f);
	if (vf) {
		FourCC id = vf->GetVariableID();
		switch (id) {
			case MAKE_FOURCC('I','T','Y','P'):
				helpLine1 = "Instrument type";
				helpLine2 = "Switch between Sample/SF2/VST3/LV2/MidiOut/AudioIn";
				break;
			case AIP_INPUTDEVICE:
				helpLine1 = "Audio input device";
				helpLine2 = "Select which hardware input to capture from";
				break;
			case AIP_VOLUME:
				helpLine1 = "Input volume (00-FF)";
				helpLine2 = "Scales the captured audio level";
				break;
			case AIP_PAN:
				helpLine1 = "Input pan (00=L 7F=C FE=R)";
				helpLine2 = "Stereo positioning of captured audio";
				break;
			case AIP_MIDIDEVICE:
				helpLine1 = "MIDI output device";
				helpLine2 = "Send notes/CCs to external gear";
				break;
			case AIP_MIDICHANNEL:
				helpLine1 = "MIDI channel (00-0F)";
				helpLine2 = "Channel for note/CC output";
				break;
			case AIP_NOTELENGTH:
				helpLine1 = "Auto note-off after N ticks";
				helpLine2 = "00=infinite (manual stop only)";
				break;
			case AIP_TRANSPOSE:
				helpLine1 = "Semitone offset for MIDI notes";
				helpLine2 = "-48 to +48";
				break;
			case AIP_TABLEAUTO:
				helpLine1 = "Table automation on/off";
				helpLine2 = "Auto-play table on each note start";
				break;
			case AIP_TABLE:
				helpLine1 = "Table assignment";
				helpLine2 = "OFF or 00-7F, press A to pick next free";
				break;
		}
	}

	GUIPoint anchor = GetAnchor();
	int helpY = anchor._y + 16;
	GUITextProperties props;
	SetColor(CD_HILITE1);
	DrawString(anchor._x, helpY, helpLine1, props);
	SetColor(CD_NORMAL);
	DrawString(anchor._x, helpY + 1, helpLine2, props);

	// Draw current input device name
	int i = viewData_->currentInstrument_;
	InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
	I_Instrument *instr = bank->GetInstrument(i);
	AudioInInstrument *instrument = (AudioInInstrument *)instr;
	Variable *vid = instrument->FindVariable(AIP_INPUTDEVICE);
	int devIdx = vid ? vid->GetInt() : 0;
	const char *devName = instrument->GetInputDeviceName(devIdx);
	if (devName && devName[0]) {
		SetColor(CD_HILITE2);
		DrawString(anchor._x, helpY + 3, devName, props);
	}

	// Draw MIDI device name
	Variable *vmd = instrument->FindVariable(AIP_MIDIDEVICE);
	int mdevIdx = vmd ? vmd->GetInt() : 0;
	const char *mdevName = instrument->GetMidiDeviceName(mdevIdx);
	if (mdevName && mdevName[0]) {
		SetColor(CD_HILITE2);
		DrawString(anchor._x, helpY + 4, mdevName, props);
	}
}

void InstrumentView::drawMidiOutHelp() {
	InstrumentType it = getInstrumentType();
	if (it != IT_MIDIOUT) return;

	UIField *f = GetFocus();
	if (!f) return;

	const char *helpLine1 = "";
	const char *helpLine2 = "";

	UIIntVarField *vf = dynamic_cast<UIIntVarField *>(f);
	if (vf) {
		FourCC id = vf->GetVariableID();
		switch (id) {
			case MAKE_FOURCC('I','T','Y','P'):
				helpLine1 = "Instrument type";
				helpLine2 = "Switch between Sample/LV2/SF2/VST3/MidiOut";
				break;
			case MOIP_DEVICE:
				helpLine1 = "MIDI output device";
				helpLine2 = "Select which hardware output to use";
				break;
			case MOIP_CHANNEL:
				helpLine1 = "MIDI channel (00-0F)";
				helpLine2 = "Channel the notes/CCs are sent on";
				break;
			case MOIP_NOTELENGTH:
				helpLine1 = "Auto note-off after N ticks";
				helpLine2 = "00=infinite (manual stop only)";
				break;
			case MOIP_TRANSPOSE:
				helpLine1 = "Semitone offset applied to notes";
				helpLine2 = "-48 to +48";
				break;
			case MOIP_CLOCK:
				helpLine1 = "Send MIDI clock (F8) to device";
				helpLine2 = "Syncs external gear to tracker tempo";
				break;
			case MOIP_TRANSPORT:
				helpLine1 = "Send MIDI start/stop to device";
				helpLine2 = "FA on play, FC on stop";
				break;
			case MOIP_TABLEAUTO:
				helpLine1 = "Table automation on/off";
				helpLine2 = "Auto-play table on each note start";
				break;
			case MOIP_TABLE:
				helpLine1 = "Table assignment";
				helpLine2 = "OFF or 00-7F, press A to pick next free";
				break;
		}
	}

	GUIPoint anchor = GetAnchor();
	int helpY = anchor._y + 16;
	GUITextProperties props;
	SetColor(CD_HILITE1);
	DrawString(anchor._x, helpY, helpLine1, props);
	SetColor(CD_NORMAL);
	DrawString(anchor._x, helpY + 1, helpLine2, props);

	// Draw current device name (updated live each frame)
	int i = viewData_->currentInstrument_;
	InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
	I_Instrument *instr = bank->GetInstrument(i);
	MidiOutInstrument *instrument = (MidiOutInstrument *)instr;
	Variable *vd = instrument->FindVariable(MOIP_DEVICE);
	int devIdx = vd ? vd->GetInt() : 0;
	const char *devName = instrument->GetDeviceName(devIdx);
	if (devName && devName[0]) {
		SetColor(CD_HILITE2);
		DrawString(anchor._x, helpY + 3, devName, props);
	}
}

void InstrumentView::drawMidiInputInfo() {
	InstrumentType it = getInstrumentType();
	if (it == IT_MIDI || it == IT_MIDIOUT || it == IT_AUDIOIN) return;

	int i = viewData_->currentInstrument_;
	InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
	I_Instrument *instr = bank->GetInstrument(i);
	if (!instr) return;

	Variable *vDev = instr->FindVariable(IMDI);
	if (!vDev) return;
	int devIdx = vDev->GetInt();
	const char *name = getMidiInDeviceName(devIdx);
	if (!name || !name[0]) return;

	// Truncate name to fit the remaining screen width
	char buf[60];
	snprintf(buf, sizeof(buf), "%.56s", name);

	// Find the Y position of the IMDI field by scanning UIFields
	GUIPoint pos;
	pos._x = 22;  // after "midi:N  ch:XX"
	bool found = false;
	IteratorPtr<UIField> fit(T_SimpleList<UIField>::GetIterator());
	for (fit->Begin(); !fit->IsDone(); fit->Next()) {
		UIIntVarField *vf = dynamic_cast<UIIntVarField *>(&fit->CurrentItem());
		if (vf && vf->GetVariableID() == IMDI) {
			pos._y = vf->GetPosition()._y;
			found = true;
			break;
		}
	}
	if (!found) return;

	GUITextProperties props;
	SetColor(CD_HILITE2);
	DrawString(pos._x, pos._y, buf, props);
}

void InstrumentView::DrawView() {

	// Process any pending type switch before drawing to avoid showing stale UI
	if (pendingTypeInstrumentIdx_ != -1) {
		int idx = pendingTypeInstrumentIdx_;
		pendingTypeInstrumentIdx_ = -1;
		
		// Stop playback before switching type to prevent crash from
		// the audio thread rendering a deleted instrument
		Player *player = Player::GetInstance();
		if (player && player->IsRunning()) {
			player->Stop();
		}
		InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
		if (idx >= 0 && idx < MAX_INSTRUMENT_COUNT) {
			bank->SetInstrumentType(idx, pendingType_);
			// Old instrument is now deleted. Clear current_ so onInstrumentChange
			// treats this as a fresh instrument (old=nullptr, skips stale cleanup,
			// properly adds observer on the new instrument).
			current_ = nullptr;
			onInstrumentChange();
		}
	}

	Clear() ;
    View::EnableNotification();

    GUITextProperties props;
    GUIPoint pos = GetTitlePosition();

    // Draw title

    char title[20];
    SetColor(CD_NORMAL);
    sprintf(title, "Instrument %2.2X", viewData_->currentInstrument_);
    DrawString(pos._x, pos._y, title, props);

    // Draw fields

    FieldView::Redraw();
    drawMidiInputInfo();
    drawMidiOutHelp();
    drawAudioInHelp();
    drawMap() ;
}

void InstrumentView::OnFocus() { onInstrumentChange(); }

void InstrumentView::OnLV2PluginSelected() {
	// Reset scroll offset when new plugin is loaded
	lv2ScrollOffset_ = 0;
	onInstrumentChange();
	isDirty_ = true;
}

void InstrumentView::OnSF2Selected() {
	onInstrumentChange();
	isDirty_ = true;
}

void InstrumentView::OnVST3PluginSelected() {
	vst3ScrollOffset_ = 0;
	onInstrumentChange();
	isDirty_ = true;
}

void InstrumentView::OnSavePreset(const std::string &name) {
	int i = viewData_->currentInstrument_;
	InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
	I_Instrument *instr = bank->GetInstrument(i);
	if (!instr) return;

	std::string dir;
	const char *ext = nullptr;
	bool saved = false;

	if (instr->GetType() == IT_LV2) {
		LV2Instrument *lv2 = (LV2Instrument *)instr;
		if (!lv2->canSavePreset()) return;
		dir = lv2->getCurrentBankDirectory();
		ext = lv2->getPresetExtension();
		if (dir.empty() || !ext) {
			MessageBox *mb = new MessageBox(*this, "No bank directory available", MBBF_OK);
			DoModal(mb);
			return;
		}
		std::string filePath = dir + "/" + name + ext;
		if (lv2->savePresetToFile(filePath)) {
			lv2->refreshPresets();
			saved = true;
		}
	} else if (instr->GetType() == IT_VST3) {
		VST3Instrument *vst3 = (VST3Instrument *)instr;
		if (!vst3->canSavePreset()) return;
		dir = vst3->getCurrentBankDirectory();
		ext = vst3->getPresetExtension();
		if (dir.empty() || !ext) {
			MessageBox *mb = new MessageBox(*this, "No bank directory available", MBBF_OK);
			DoModal(mb);
			return;
		}
		std::string filePath = dir + "/" + name + ext;
		if (vst3->savePresetToFile(filePath)) {
			vst3->refreshPresets();
			saved = true;
		}
	} else {
		return;
	}

	if (saved) {
		onInstrumentChange();
		isDirty_ = true;
	} else {
		MessageBox *mb = new MessageBox(*this, "Failed to save preset", MBBF_OK);
		DoModal(mb);
	}
}

void InstrumentView::Update(Observable &o,I_ObservableData *d) {    // Handle action field clicks (e.g. LV2 load action)
    UIActionField *action = dynamic_cast<UIActionField *>(&o);
    if (action) {
        if (d) {
#ifdef _64BIT
            unsigned int fourcc = *(unsigned int *)d;
#else
            unsigned int fourcc = (unsigned int)(intptr_t)d;
#endif
            if (fourcc == ACTION_LOAD_LV2) {
                InstrumentType it = getInstrumentType();
                if (it == IT_LV2) {
                    ImportLV2Dialog *dialog = new ImportLV2Dialog(*this);
                    DoModal(dialog, LV2PluginSelectCallback);
                }
                return;
            }
            if (fourcc == ACTION_LOAD_SF2) {
                InstrumentType it = getInstrumentType();
                if (it == IT_SOUNDFONT) {
                    ImportSoundFontDialog *dialog = new ImportSoundFontDialog(*this);
                    DoModal(dialog, SF2SelectCallback);
                }
                return;
            }
            if (fourcc == ACTION_LOAD_VST3) {
                InstrumentType it = getInstrumentType();
                if (it == IT_VST3) {
                    ImportVST3Dialog *dialog = new ImportVST3Dialog(*this);
                    DoModal(dialog, VST3PluginSelectCallback);
                }
                return;
            }
            if (fourcc == ACTION_SAVE_PRESET) {
                InstrumentType it = getInstrumentType();
                if (it == IT_LV2 || it == IT_VST3) {
                    SavePresetDialog *dialog = new SavePresetDialog(*this);
                    DoModal(dialog, SavePresetCallback);
                }
                return;
            }
            if (fourcc == ACTION_EDIT_SAMPLE) {
                InstrumentType it = getInstrumentType();
                if (it == IT_SAMPLE) {
                    ViewType vt = VT_SAMPLE_EDITOR;
                    ViewEvent ve(VET_SWITCH_VIEW, &vt);
                    SetChanged();
                    NotifyObservers(&ve);
                }
                return;
            }
            if (fourcc == ACTION_REC_SAMPLE) {
                InstrumentType it = getInstrumentType();
                if (it == IT_SAMPLE) {
                    RecordSampleDialog *dialog = new RecordSampleDialog(*this);
                    DoModal(dialog, RecordSampleCallback);
                }
                return;
            }
        }
    }
    // Check for a change in the 'type' variable so we can switch instrument types on demand
    int i = viewData_->currentInstrument_;
    InstrumentBank *bank = viewData_->project_->GetInstrumentBank();
    I_Instrument *instr = bank->GetInstrument(i);
    if (instr) {
        // Check ITYP type switch FIRST — must take priority over preset changes
        Variable *tv = instr->FindVariable(MAKE_FOURCC('I','T','Y','P'));
        if (tv) {
            int val = tv->GetInt();
            // val==0 => Sample, val==1 => SF2 (IT_SOUNDFONT), val==2 => VST3, val==3 => LV2, val==4 => MidiOut, val==5 => AudioIn
            InstrumentType targetType = IT_SAMPLE;
            if (val == 1) targetType = IT_SOUNDFONT;
            else if (val == 2) targetType = IT_VST3;
            else if (val == 3) targetType = IT_LV2;
            else if (val == 4) targetType = IT_MIDIOUT;
            else if (val == 5) targetType = IT_AUDIOIN;

            if (instr->GetType() != targetType) {
                // Defer changing instrument type until outside of the variable notification
                pendingTypeInstrumentIdx_ = i;
                pendingType_ = targetType;
                
                isDirty_ = true;
                return;
            }
        }

		// Handle changes to the instrument-level effect slot (IFXS).
		Variable *var = dynamic_cast<Variable *>(&o);
		if (var && var->GetID() == IFXS) {
			int slot = var->GetInt();
			int idx = viewData_->currentInstrument_;
			InstrumentBank *bank2 = viewData_->project_->GetInstrumentBank();
			I_Instrument *instr2 = bank2->GetInstrument(idx);
			if (instr2 && instr2->GetType() == IT_AUDIOIN) {
				AudioInInstrument *ai = (AudioInInstrument *)instr2;
				if (slot >= 0 && viewData_->project_) {
					I_Effect *eff = viewData_->project_->GetEffect(slot);
					if (eff && !eff->IsEmpty()) ai->SetEffect(eff, 255);
					else ai->ClearEffect();
				} else {
					ai->ClearEffect();
				}
			}
			return;
		}

        // Handle SF2 preset changes (only if type is not changing)
        if (instr->GetType() == IT_SOUNDFONT) {
            SoundFontInstrument *sfi = (SoundFontInstrument *)instr;
            Variable *pv = sfi->FindVariable(SFIP_PRESET);
            if (pv && sfi->GetCurrentPreset() != pv->GetInt()) {
                
                sfi->SelectPreset(pv->GetInt());
                onInstrumentChange();
                isDirty_ = true;
                return;
            }
        }

        // Handle VST3 bank/preset changes
        if (instr->GetType() == IT_VST3) {
            VST3Instrument *vst3 = (VST3Instrument *)instr;
            Variable *bv = vst3->FindVariable(VST3IP_BANK);
            if (bv && vst3->GetCurrentBank() != bv->GetInt()) {
                vst3->SetCurrentBank(bv->GetInt());
                // Reset preset to 0 when bank changes
                Variable *pv = vst3->FindVariable(VST3IP_PRESET);
                if (pv) pv->SetInt(0);
                vst3->SetPreset(0);
                onInstrumentChange();
                isDirty_ = true;
                return;
            }
            Variable *pv = vst3->FindVariable(VST3IP_PRESET);
            if (pv && vst3->GetCurrentPreset() != pv->GetInt()) {
                vst3->SetPreset(pv->GetInt());
                onInstrumentChange();
                isDirty_ = true;
                return;
            }
        }

        // Handle LV2 bank/preset changes
        if (instr->GetType() == IT_LV2) {
            LV2Instrument *lv2 = (LV2Instrument *)instr;
            Variable *bv = lv2->FindVariable(LV2IP_BANK);
            if (bv && lv2->GetCurrentBank() != bv->GetInt()) {
                lv2->SetCurrentBank(bv->GetInt());
                Variable *pv = lv2->FindVariable(LV2IP_PRESET);
                if (pv) pv->SetInt(0);
                lv2->SetPreset(0);
                onInstrumentChange();
                isDirty_ = true;
                return;
            }
            Variable *pv = lv2->FindVariable(LV2IP_PRESET);
            if (pv && lv2->GetCurrentPreset() != pv->GetInt()) {
                lv2->SetPreset(pv->GetInt());
                onInstrumentChange();
                isDirty_ = true;
                return;
            }
        }
    }

    // Only rebuild if the notification came from the instrument itself (not a WatchedVariable)
    // WatchedVariable notifications for type/preset are handled above; anything else
    // (e.g. the instrument notifying after load) triggers a rebuild.
    WatchedVariable *wv = dynamic_cast<WatchedVariable *>(&o);
    if (!wv) {
        onInstrumentChange() ;
        isDirty_ = true;
    }
}
