#include "InstrumentView.h"
#include "Application/Instruments/MidiInstrument.h"
#include "Application/Instruments/SampleInstrument.h"
#include "Application/Instruments/LV2Instrument.h"
#include "Application/Instruments/SoundFontInstrument.h"
#include "Application/Instruments/VST3Instrument.h"
#include "Application/Instruments/SamplePool.h"
#include "Application/Model/Config.h"
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
#include "System/System/System.h"
#include "System/Console/Trace.h"
#include "Application/Player/Player.h"
#include <map>
#include <vector>
#include <string>

#define ACTION_LOAD_LV2 MAKE_FOURCC('L','V','2','L')
#define ACTION_LOAD_SF2 MAKE_FOURCC('S','F','2','L')
#define ACTION_LOAD_VST3 MAKE_FOURCC('V','3','L','D')

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

InstrumentView::InstrumentView(GUIWindow &w,ViewData *data):FieldView(w,data) {

	project_=data->project_ ;
	lastFocusID_=0 ;
	current_=0 ;
	lv2ScrollOffset_=0 ;
	pendingTypeInstrumentIdx_ = -1;
	pendingType_ = IT_SAMPLE;
	lv2LoadField_ = nullptr;
	sf2LoadField_ = nullptr;
	vst3LoadField_ = nullptr;
	vst3ScrollOffset_ = 0;
	// Initialize lastType_ to mirror instrument types
	for (int i = 0; i < MAX_INSTRUMENT_COUNT; ++i) {
		I_Instrument* instr = data->project_->GetInstrumentBank()->GetInstrument(i);
		if (instr && instr->FindVariable(MAKE_FOURCC('I','T','Y','P')))
			lastType_[i] = instr->FindVariable(MAKE_FOURCC('I','T','Y','P'))->GetInt();
		else {
			if (instr && instr->GetType() == IT_LV2) lastType_[i] = 1;
			else if (instr && instr->GetType() == IT_SOUNDFONT) lastType_[i] = 2;
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
	Trace::Log("IVIEW", "onInstrumentChange: inst=%d type=%d current=%p old=%p", i, (int)it, current_, old);

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
	} ;

	SetFocus(T_SimpleList<UIField>::GetFirst()) ;
	IteratorPtr<UIField> it2(T_SimpleList<UIField>::GetIterator()) ;
	for (it2->Begin();!it2->IsDone();it2->Next()) {
        UIIntVarField &field=(UIIntVarField &)it2->CurrentItem() ;
        if (field.GetVariableID()==lastFocusID_) {
            SetFocus(&field) ;
            break ;
        }
    } ;
	if (current_!=old) {
		current_->AddObserver(*this) ;
	}
} ;

void InstrumentView::fillSampleParameters() {

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	SampleInstrument *instrument=(SampleInstrument *)instr  ;
	GUIPoint position=GetAnchor() ;

    // Local variables used to create fields
    Variable *v = nullptr;
    UIIntVarField *f1 = nullptr;
// Type selector: Sample vs LV2 vs SF2 vs VST3 instrument
    Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
    if (!tv) {
        static char *instrTypes[] = { (char*)"Sample", (char*)"LV2", (char*)"SF2", (char*)"VST3" } ;
        WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 4, 0);
        instrument->Insert(wtv);
        tv = wtv;
    }
    UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 3, 1, 7);
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
    position._y += 2;
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
	f1=new UIIntVarField(position,*v,"slices: %2.2X",1,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_START) ;
	f1=new UIBigHexVarField(position,*v,7,"start: %7.7X",0,instrument->GetSampleSize()-1,16) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_LOOPSTART) ;
	f1=new UIBigHexVarField(position,*v,7,"loop start: %7.7X",0,instrument->GetSampleSize()-1,16) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_END) ;
	f1=new UIBigHexVarField(position,*v,7,"loop end: %7.7X",0,instrument->GetSampleSize()-1,16) ;
	T_SimpleList<UIField>::Insert(f1) ;

	v=instrument->FindVariable(SIP_TABLEAUTO) ;
	position._y+=2 ;
	UIIntVarField *f2=new UIIntVarField(position,*v,"automation: %s",0,1,1,1) ;
	T_SimpleList<UIField>::Insert(f2) ;

	position._y+=1 ;
	v=instrument->FindVariable(SIP_TABLE) ;
	f1=new UIIntVarOffField(position,*v,"table: %2.2X",0x00,0x7F,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

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
	f1->SetFocus() ;

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

void InstrumentView::fillLV2Parameters() {

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	LV2Instrument *instrument=(LV2Instrument *)instr  ;
	GUIPoint position=GetAnchor() ;
	
	// Constants for two-column layout
	const int COL_WIDTH = 27;      // Width of each column (25 + 2 space gap)
	const int MAX_ROWS = 17;       // Max rows for parameters (leaving room for header + footer)
	const int PARAMS_PER_PAGE = MAX_ROWS * 2;  // Two columns

    // Type selector: Sample vs LV2 vs SF2 vs VST3 instrument (keep accessible to switch back)
    Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
    if (!tv) {
        static char *instrTypes[] = { (char*)"Sample", (char*)"LV2", (char*)"SF2", (char*)"VST3" } ;
        WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 4, 1);
        instrument->Insert(wtv);
        tv = wtv;
    }
    UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 3, 1, 7);
    T_SimpleList<UIField>::Insert(typeField);
    position._y += 1;

    // Ensure this view observes changes to the 'type' variable so we can react immediately
    if (WatchedVariable *wtv = dynamic_cast<WatchedVariable *>(tv)) {
        // Avoid duplicate observers
        wtv->RemoveObserver(*this);
        wtv->AddObserver(*this);
    }

    // Display help text at top-right (compute rightmost x based on visible logical width)
    const char *helpText = "L to change pages";
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
			int baseX = 0;  // Start at left edge of screen
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
                Trace::Debug("InstrumentView: param field created idx=%d name=%s varIsWatched=%d varID=%u", p, param->name.c_str(), wv_check ? 1 : 0, param->variable->GetID());

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

    // Footer controls - stacked vertically since LV2 has no note blocks
    Variable *v=instrument->FindVariable(LV2IP_VOLUME) ;
	UIIntVarField* f1=new UIIntVarField(position,*v,"volume: %2.2X",0,0xFF,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;
	f1->SetFocus() ;

	position._y += 1;
	v=instrument->FindVariable(LV2IP_PAN) ;
	f1=new UIIntVarField(position,*v,"pan: %2.2X",0,0xFE,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

	position._y += 1;
	v=instrument->FindVariable(LV2IP_TABLEAUTO) ;
	UIIntVarField *f2=new UIIntVarField(position,*v,"automation: %s",0,1,1,1) ;
	T_SimpleList<UIField>::Insert(f2) ;

	position._y += 1;
	v=instrument->FindVariable(LV2IP_TABLE) ;
	f1=new UIIntVarOffField(position,*v,"table: %2.2X",0,0x7F,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;

} ;

void InstrumentView::fillSoundFontParameters() {

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	SoundFontInstrument *instrument=(SoundFontInstrument *)instr  ;
	GUIPoint position=GetAnchor() ;

	// Local variables used to create fields
	Variable *v = nullptr;
	UIIntVarField *f1 = nullptr;

	// Type selector: Sample vs LV2 vs SF2 vs VST3
	Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
	if (!tv) {
		static char *instrTypes[] = { (char*)"Sample", (char*)"LV2", (char*)"SF2", (char*)"VST3" } ;
		WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 4, 2);
		instrument->Insert(wtv);
		tv = wtv;
	}
	UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 3, 1, 7);
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

	// Volume
	v = instrument->FindVariable(SFIP_VOLUME);
	f1 = new UIIntVarField(position, *v, "volume: %2.2X", 0, 0xFF, 1, 0x10);
	T_SimpleList<UIField>::Insert(f1);

	position._y += 1;

	// Pan
	v = instrument->FindVariable(SFIP_PAN);
	f1 = new UIIntVarField(position, *v, "pan: %2.2X", 0, 0xFE, 1, 0x10);
	T_SimpleList<UIField>::Insert(f1);

	position._y += 2;

	// Table automation
	v = instrument->FindVariable(SFIP_TABLEAUTO);
	UIIntVarField *f2 = new UIIntVarField(position, *v, "automation: %s", 0, 1, 1, 1);
	T_SimpleList<UIField>::Insert(f2);

	position._y += 1;

	// Table
	v = instrument->FindVariable(SFIP_TABLE);
	f1 = new UIIntVarOffField(position, *v, "table: %2.2X", 0x00, 0x7F, 1, 0x10);
	T_SimpleList<UIField>::Insert(f1);
}

void InstrumentView::fillVST3Parameters() {

	int i=viewData_->currentInstrument_ ;
	InstrumentBank *bank=viewData_->project_->GetInstrumentBank() ;
	I_Instrument *instr=bank->GetInstrument(i) ;
	VST3Instrument *instrument=(VST3Instrument *)instr ;
	GUIPoint position=GetAnchor() ;

	const int COL_WIDTH = 27;
	const int MAX_ROWS = 17;
	const int PARAMS_PER_PAGE = MAX_ROWS * 2;

	// Type selector
	Variable *tv = instrument->FindVariable(MAKE_FOURCC('I','T','Y','P')) ;
	if (!tv) {
		static char *instrTypes[] = { (char*)"Sample", (char*)"LV2", (char*)"SF2", (char*)"VST3" } ;
		WatchedVariable *wtv = new WatchedVariable("type", MAKE_FOURCC('I','T','Y','P'), instrTypes, 4, 3);
		instrument->Insert(wtv);
		tv = wtv;
	}
	UIIntVarField *typeField = new UIIntVarField(position, *tv, "Type: %s", 0, 3, 1, 7);
	T_SimpleList<UIField>::Insert(typeField);
	position._y += 1;

	if (WatchedVariable *wtv = dynamic_cast<WatchedVariable *>(tv)) {
		wtv->RemoveObserver(*this);
		wtv->AddObserver(*this);
	}

	// Page scroll help
	const char *helpText = "L to change pages";
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
			int baseX = 0;
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

	// Footer controls
	Variable *v = instrument->FindVariable(VST3IP_VOLUME);
	UIIntVarField *f1 = new UIIntVarField(position, *v, "volume: %2.2X", 0, 0xFF, 1, 0x10);
	T_SimpleList<UIField>::Insert(f1);
	f1->SetFocus();

	position._y += 1;
	v = instrument->FindVariable(VST3IP_PAN);
	f1 = new UIIntVarField(position, *v, "pan: %2.2X", 0, 0xFE, 1, 0x10);
	T_SimpleList<UIField>::Insert(f1);

	position._y += 1;
	v = instrument->FindVariable(VST3IP_TABLEAUTO);
	UIIntVarField *f2 = new UIIntVarField(position, *v, "automation: %s", 0, 1, 1, 1);
	T_SimpleList<UIField>::Insert(f2);

	position._y += 1;
	v = instrument->FindVariable(VST3IP_TABLE);
	f1 = new UIIntVarOffField(position, *v, "table: %2.2X", 0, 0x7F, 1, 0x10);
	T_SimpleList<UIField>::Insert(f1);
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
                v->SetInt(-1) ;
                isDirty_=true ;
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


void InstrumentView::DrawView() {

	// Process any pending type switch before drawing to avoid showing stale UI
	if (pendingTypeInstrumentIdx_ != -1) {
		int idx = pendingTypeInstrumentIdx_;
		pendingTypeInstrumentIdx_ = -1;
		Trace::Log("IVIEW", "DrawView: processing pending type switch idx=%d type=%d", idx, (int)pendingType_);
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
            // val==0 => Sample, val==1 => LV2, val==2 => SF2, val==3 => VST3
            InstrumentType targetType = IT_SAMPLE;
            if (val == 1) targetType = IT_LV2;
            else if (val == 2) targetType = IT_SOUNDFONT;
            else if (val == 3) targetType = IT_VST3;

            if (instr->GetType() != targetType) {
                // Defer changing instrument type until outside of the variable notification
                pendingTypeInstrumentIdx_ = i;
                pendingType_ = targetType;
                Trace::Log("IVIEW", "Update: deferring type switch inst=%d from=%d to=%d", i, (int)instr->GetType(), (int)targetType);
                isDirty_ = true;
                return;
            }
        }

        // Handle SF2 preset changes (only if type is not changing)
        if (instr->GetType() == IT_SOUNDFONT) {
            SoundFontInstrument *sfi = (SoundFontInstrument *)instr;
            Variable *pv = sfi->FindVariable(SFIP_PRESET);
            if (pv && sfi->GetCurrentPreset() != pv->GetInt()) {
                Trace::Log("SF2", "Preset change detected: var=%d current=%d", pv->GetInt(), sfi->GetCurrentPreset());
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
