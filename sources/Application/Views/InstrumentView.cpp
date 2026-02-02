#include "InstrumentView.h"
#include "Application/Instruments/MidiInstrument.h"
#include "Application/Instruments/SampleInstrument.h"
#include "Application/Instruments/LV2Instrument.h"
#include "Application/Instruments/SamplePool.h"
#include "Application/Model/Config.h"
#include "BaseClasses/UIBigHexVarField.h"
#include "BaseClasses/UIIntVarOffField.h"
#include "BaseClasses/UINoteVarField.h"
#include "BaseClasses/UIStaticField.h"
#include "BaseClasses/UILV2ParameterField.h"
#include "Foundation/Variables/Variable.h"
#include "ModalDialogs/ImportSampleDialog.h"
#include "ModalDialogs/ImportLV2Dialog.h"
#include "ModalDialogs/MessageBox.h"
#include "System/System/System.h"
#include <map>
#include <vector>
#include <string>

// Callback for LV2 plugin selection dialog
static void LV2PluginSelectCallback(View &v, ModalView &dialog) {
	InstrumentView &iv = (InstrumentView &)v;
	iv.OnLV2PluginSelected();
}

InstrumentView::InstrumentView(GUIWindow &w,ViewData *data):FieldView(w,data) {

	project_=data->project_ ;
	lastFocusID_=0 ;
	current_=0 ;
	lv2ScrollOffset_=0 ;
	onInstrumentChange() ;
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

	if (current_!=old && old!=0) {
		old->RemoveObserver(*this) ;
	} ;
	T_SimpleList<UIField>::Empty() ;

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

//	position._y+=View::fieldSpaceHeight_;
	Variable *v=instrument->FindVariable(SIP_SAMPLE) ;
	SamplePool *sp=SamplePool::GetInstance() ;
	UIIntVarField *f1=new UIIntVarField(position,*v,"sample: %s",0,sp->GetNameListSize()-1,1,0x10) ;
	T_SimpleList<UIField>::Insert(f1) ;
	f1->SetFocus() ;

    position._y += 1;
#ifdef FFMPEG_ENABLED
    v = instrument->FindVariable(SIP_PRINTFX);
    f1 = new UIIntVarField(position, *v, "%s", 0, 3, 1, 2);
    T_SimpleList<UIField>::Insert(f1) ;

    position._x += 7;
    v = instrument->FindVariable(SIP_IR_WET);
    f1 = new UIIntVarField(position, *v, "wet:%d%%", 0, 100, 1, 10);
    T_SimpleList<UIField>::Insert(f1);
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

	// Display help text at top
	UIStaticField *helpField=new UIStaticField(position, "Press B to open LV2 list, L to scroll") ;
	T_SimpleList<UIField>::Insert(helpField) ;
	position._y+=1;

	// Display the plugin selector (static field)
	if (instrument->IsEmpty()) {
		strcpy(lv2PluginLabel_, "load list");
	} else {
		snprintf(lv2PluginLabel_, 80, "plugin: %s", instrument->GetName());
	}
	UIStaticField *sf=new UIStaticField(position, lv2PluginLabel_) ;
	T_SimpleList<UIField>::Insert(sf) ;

	position._y+=1;
	
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
			// For variable-based fields
			if (mask&EPBM_A) {
				UIIntVarField *field=(UIIntVarField *)GetFocus() ;
				Variable &v=field->GetVariable() ;
				switch(v.GetID()) {
					case SIP_SAMPLE:
					 {
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
		mask&=(0xFFFF-(EPBM_A|EPBM_L)) ;
	} ;

	if (viewMode_==VM_SELECTION) {
	} else {
		viewMode_=VM_NORMAL ;
	}

	FieldView::ProcessButtonMask(mask) ;

    Player *player=Player::GetInstance() ;
    
	// B button - LV2 plugin selection (when not holding B as modifier)
	if (mask == EPBM_B) {
		InstrumentType it=getInstrumentType() ;
		if (it==IT_LV2) {
			ImportLV2Dialog *dialog=new ImportLV2Dialog(*this) ;
			DoModal(dialog, LV2PluginSelectCallback) ;
			return ;
		}
	}
	
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
            FourCC varID = ((UIIntVarField *)GetFocus())->GetVariableID();
            if ((varID == SIP_TABLE) || (varID == MIP_TABLE) ||
                (varID == SIP_SAMPLE) || (varID == SIP_PRINTFX)) {
                viewMode_ = VM_NEW;
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

                if (mask & EPBM_START) {
                    player->OnStartButton(PM_PHRASE, viewData_->songX_, true,
                                          viewData_->chainRow_);
                }
            } else if (mask & EPBM_L) {
                // L shoulder - scroll LV2 parameters with wrap-around
                InstrumentType it=getInstrumentType() ;
                const int PARAMS_PER_PAGE = 36;  // 18 rows x 2 columns
                if (it==IT_LV2) {
                    LV2Instrument *lv2instr=(LV2Instrument *)current_ ;
                    if (!lv2instr->IsEmpty() && lv2instr->GetParameterCount() > PARAMS_PER_PAGE) {
                        lv2ScrollOffset_ += 18;  // Scroll by one column
                        // Wrap around when hitting the end
                        if (lv2ScrollOffset_ >= lv2instr->GetParameterCount()) {
                            lv2ScrollOffset_ = 0;
                        }
                        onInstrumentChange();
                        isDirty_=true;
                        return; // Don't process other L commands
                    }
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

    UIIntVarField *field = (UIIntVarField *)GetFocus();
    if (field) {
	   lastFocusID_=field->GetVariableID() ;
    }

} ;

void InstrumentView::DrawView() {

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
} ;

void InstrumentView::OnFocus() { onInstrumentChange(); }

void InstrumentView::OnLV2PluginSelected() {
	// Reset scroll offset when new plugin is loaded
	lv2ScrollOffset_ = 0;
	onInstrumentChange();
	isDirty_ = true;
}

void InstrumentView::Update(Observable &o,I_ObservableData *d) {
	onInstrumentChange() ;
}
