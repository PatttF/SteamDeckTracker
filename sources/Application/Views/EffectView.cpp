#include "EffectView.h"
#include "Application/Instruments/I_Effect.h"
#include "Application/Instruments/LV2Effect.h"
#include "Application/Instruments/VST3Effect.h"
#include "BaseClasses/UIVST3EffectParameterField.h"
#include "Application/Model/Config.h"
#include "BaseClasses/UIIntVarField.h"
#include "BaseClasses/UIIntVarOffField.h"
#include "BaseClasses/UILV2EffectParameterField.h"
#include "BaseClasses/UIStaticField.h"
#include "BaseClasses/UIActionField.h"
#include "Foundation/Variables/Variable.h"
#include "Foundation/Variables/WatchedVariable.h"
#include "ModalDialogs/ImportLV2EffectDialog.h"
#include "ModalDialogs/ImportVST3EffectDialog.h"
#include "ModalDialogs/MessageBox.h"
#include "Application/Player/Player.h"
#include "System/Console/Trace.h"
#include <cstring>
#include <vector>
#include <string>
#include <map>

#define ACTION_LOAD_EFFECT MAKE_FOURCC('E','F','L','D')
#define EFTP MAKE_FOURCC('E','F','T','P')

// Effect type names for the picker (index 0 = VST3, index 1 = LV2)
static char *effectTypeNames[] = { (char*)"VST3", (char*)"LV2" };
static const int NUM_EFFECT_TYPES = 2;

// Callback for effect plugin selection dialog
static void EffectPluginSelectCallback(View &v, ModalView &dialog) {
    EffectView &ev = (EffectView &)v;
    ev.OnEffectPluginSelected();
}

EffectView::EffectView(GUIWindow &w, ViewData *data) : FieldView(w, data) {
    project_ = data->project_;
    currentEffect_ = 0;
    scrollOffset_ = 0;
    current_ = nullptr;
    loadField_ = nullptr;
    currentTypeIndex_ = 0;  // VST3 default
    pendingTypeEffectIdx_ = -1;
    pendingEffectType_ = ET_VST3;
    memset(pluginLabel_, 0, sizeof(pluginLabel_));
    memset(paramText_, 0, sizeof(paramText_));
}

EffectView::~EffectView() {
}

void EffectView::onEffectChange() {
    ClearFocus();

    I_Effect *old = current_;
    current_ = project_->GetEffect(currentEffect_);
    if (!current_) return;

    // Sync the type picker to match the current effect's type
    if (current_->GetEffectType() == ET_VST3) {
        currentTypeIndex_ = 0;
    } else {
        currentTypeIndex_ = 1;
    }

    T_SimpleList<UIField>::Empty();
    fillEffectParameters();

    // Focus the first non-static field
    UIField *firstFocusable = nullptr;
    IteratorPtr<UIField> fit(T_SimpleList<UIField>::GetIterator());
    for (fit->Begin(); !fit->IsDone(); fit->Next()) {
        if (!fit->CurrentItem().IsStatic()) {
            firstFocusable = &fit->CurrentItem();
            break;
        }
    }
    SetFocus(firstFocusable ? firstFocusable : T_SimpleList<UIField>::GetFirst());
}

void EffectView::fillEffectParameters() {
    if (!current_) return;

    GUIPoint position = GetAnchor();

    const int COL_WIDTH = 27;
    const int MAX_ROWS = 17;
    const int PARAMS_PER_PAGE = MAX_ROWS * 2;

    // Help text at top-right
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

    position._y += 1;

    // Type picker line: left/right to switch between VST3 and LV2
    Variable *tv = current_->FindVariable(EFTP);
    if (!tv) {
        WatchedVariable *wtv = new WatchedVariable("type", EFTP, effectTypeNames, NUM_EFFECT_TYPES, currentTypeIndex_);
        current_->Insert(wtv);
        tv = wtv;
    }
    UIIntVarField *typeField = new UIIntVarField(position, *tv, "type: %s", 0, NUM_EFFECT_TYPES - 1, 1, 1);
    T_SimpleList<UIField>::Insert(typeField);
    if (WatchedVariable *wtv = dynamic_cast<WatchedVariable *>(tv)) {
        wtv->RemoveObserver(*this);
        wtv->AddObserver(*this);
    }

    position._y += 1;

    // Plugin loader line
    static char loadLabelBuf[80];
    if (current_->IsEmpty()) {
        strcpy(loadLabelBuf, "load list");
    } else {
        snprintf(loadLabelBuf, sizeof(loadLabelBuf), "plugin: %s", current_->GetName());
    }
    UIActionField *af = new UIActionField(loadLabelBuf, ACTION_LOAD_EFFECT, position);
    T_SimpleList<UIField>::Insert(af);
    af->AddObserver(*this);
    loadField_ = af;

    position._y += 1;

    // Show bank/preset selectors for VST3 effects (only if useful presets found)
    if (!current_->IsEmpty() && current_->GetEffectType() == ET_VST3) {
        VST3Effect *vst3fx = (VST3Effect *)current_;
        if (vst3fx->GetBankCount() > 0 && vst3fx->GetPresetCount() > 1) {
            // Bank selector (if more than one bank)
            if (vst3fx->GetBankCount() > 1) {
                Variable *bv = vst3fx->FindVariable(VST3FX_BANK);
                if (!bv) {
                    int maxBank = vst3fx->GetBankCount() - 1;
                    WatchedVariable *wbv = new WatchedVariable("bank", VST3FX_BANK, 0);
                    wbv->AddObserver(*this);
                    vst3fx->Insert(wbv);
                    bv = wbv;
                }
                if (WatchedVariable *wbv = dynamic_cast<WatchedVariable *>(bv)) {
                    wbv->RemoveObserver(*this);
                    wbv->AddObserver(*this);
                }
                int maxBank = vst3fx->GetBankCount() - 1;
                if (maxBank < 0) maxBank = 0;
                UIIntVarField *bf = new UIIntVarField(position, *bv, "bank: %2.2X", 0, maxBank, 1, 0x10);
                T_SimpleList<UIField>::Insert(bf);
                position._y += 1;

                // Show bank name
                int bankIdx = vst3fx->GetCurrentBank();
                const char *bankName = vst3fx->GetBankName(bankIdx);
                snprintf(bankLabel_, sizeof(bankLabel_), "  [%s]", bankName);
                UIStaticField *bsf = new UIStaticField(position, bankLabel_);
                T_SimpleList<UIField>::Insert(bsf);
                position._y += 1;
            }

            // Preset selector
            Variable *pv = vst3fx->FindVariable(VST3FX_PRESET);
            if (!pv) {
                WatchedVariable *wpv = new WatchedVariable("preset", VST3FX_PRESET, 0);
                wpv->AddObserver(*this);
                vst3fx->Insert(wpv);
                pv = wpv;
            }
            if (WatchedVariable *wpv = dynamic_cast<WatchedVariable *>(pv)) {
                wpv->RemoveObserver(*this);
                wpv->AddObserver(*this);
            }
            // Sync variable to actual preset
            if (pv->GetInt() != vst3fx->GetCurrentPreset()) {
                pv->SetInt(vst3fx->GetCurrentPreset());
            }
            int maxPreset = vst3fx->GetPresetCount() - 1;
            if (maxPreset < 0) maxPreset = 0;
            UIIntVarField *pf = new UIIntVarField(position, *pv, "preset: %2.2X", 0, maxPreset, 1, 0x10);
            T_SimpleList<UIField>::Insert(pf);
            position._y += 1;

            // Show preset name
            int presetIdx = vst3fx->GetCurrentPreset();
            const char *presetName = vst3fx->GetPresetName(presetIdx);
            snprintf(presetLabel_, sizeof(presetLabel_), "  [%s]", presetName);
            UIStaticField *sf = new UIStaticField(position, presetLabel_);
            T_SimpleList<UIField>::Insert(sf);
            position._y += 1;
        }
    }

    // Show parameters if plugin is loaded
    if (!current_->IsEmpty()) {
        int paramCount = current_->GetParameterCount();
        if (paramCount > 0) {
            std::vector<int> displayOrder;

            if (current_->GetEffectType() == ET_LV2) {
                // Group parameters by groupName
                std::vector<std::pair<std::string, std::vector<int>>> groups;
                std::map<std::string, int> groupIndex;
                for (int p = 0; p < paramCount; p++) {
                    const EffectParameter *param = current_->GetEffectParameter(p);
                    if (!param) continue;
                    std::string grp = param->groupName.empty() ? "" : param->groupName;
                    if (groupIndex.find(grp) == groupIndex.end()) {
                        groupIndex[grp] = (int)groups.size();
                        groups.push_back({grp, {}});
                    }
                    groups[groupIndex[grp]].second.push_back(p);
                }
                for (auto &grp : groups) {
                    for (int idx : grp.second) displayOrder.push_back(idx);
                }
            } else {
                for (int p = 0; p < paramCount; p++) displayOrder.push_back(p);
            }

            int totalParams = (int)displayOrder.size();
            int startIdx = scrollOffset_;
            if (startIdx >= totalParams) startIdx = 0;
            int endIdx = startIdx + PARAMS_PER_PAGE;
            if (endIdx > totalParams) endIdx = totalParams;

            int baseY = position._y;
            int baseX = 0;
            int col = 0;
            int row = 0;
            int textIdx = 0;

            for (int i = startIdx; i < endIdx && textIdx < 40; i++, textIdx++) {
                int p = displayOrder[i];
                const EffectParameter *param = current_->GetEffectParameter(p);
                if (!param || !param->variable) continue;

                GUIPoint pos(baseX + col * COL_WIDTH, baseY + row);

                strncpy(paramText_[textIdx], param->name.c_str(), 22);
                paramText_[textIdx][22] = '\0';

                int stepSize = 1;
                int bigStep = 10;

                if (!param->scalePoints.empty()) {
                    int numPoints = (int)param->scalePoints.size();
                    if (numPoints > 1) {
                        stepSize = 127 / (numPoints - 1);
                        if (stepSize < 1) stepSize = 1;
                        bigStep = stepSize * 4;
                    }
                }

                if (current_->GetEffectType() == ET_LV2) {
                    LV2Effect *lv2fx = (LV2Effect *)current_;
                    UILV2EffectParameterField *pfield = new UILV2EffectParameterField(
                        pos, *param->variable, paramText_[textIdx],
                        lv2fx, p, 0, 127, stepSize, bigStep);
                    T_SimpleList<UIField>::Insert(pfield);
                } else {
                    VST3Effect *vst3fx = (VST3Effect *)current_;
                    const VST3PluginParameter *vp = vst3fx->GetVST3Parameter(p);
                    int maxVal = 127;
                    if (vp && vp->stepCount > 0 && vp->stepCount <= 255) {
                        maxVal = vp->stepCount;
                    }
                    UIVST3EffectParameterField *pfield = new UIVST3EffectParameterField(
                        pos, *param->variable, paramText_[textIdx],
                        vst3fx, p, 0, maxVal, stepSize, bigStep);
                    T_SimpleList<UIField>::Insert(pfield);
                }

                row++;
                if (row >= MAX_ROWS) {
                    row = 0;
                    col++;
                    if (col >= 2) break;
                }
            }

            position._y = baseY + MAX_ROWS + 1;
        } else {
            position._y += 2;
        }

        // Footer controls: volume and wet/dry
        FourCC volId = (current_->GetEffectType() == ET_LV2) ? LV2FX_VOLUME : VST3FX_VOLUME;
        FourCC wetId = (current_->GetEffectType() == ET_LV2) ? LV2FX_WETDRY : VST3FX_WETDRY;

        Variable *v = current_->FindVariable(volId);
        if (v) {
            UIIntVarField *f1 = new UIIntVarField(position, *v, "volume: %2.2X", 0, 0xFF, 1, 0x10);
            T_SimpleList<UIField>::Insert(f1);
            position._y += 1;
        }

        v = current_->FindVariable(wetId);
        if (v) {
            UIIntVarField *f1 = new UIIntVarField(position, *v, "wet/dry: %2.2X", 0, 0xFF, 1, 0x10);
            T_SimpleList<UIField>::Insert(f1);
            position._y += 1;
        }
    } else {
        position._y += 2;
    }
}

void EffectView::warpToNext(int offset) {
    int effect = currentEffect_ + offset;
    if (effect >= MAX_EFFECT_COUNT) {
        effect = effect - MAX_EFFECT_COUNT;
    }
    if (effect < 0) {
        effect = MAX_EFFECT_COUNT + effect;
    }
    currentEffect_ = effect;
    scrollOffset_ = 0;
    onEffectChange();
    isDirty_ = true;
}

void EffectView::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed) return;

    isDirty_ = false;

    // Handle A press on load field or type field
    if (viewMode_ == VM_NEW) {
        if (mask == EPBM_A) {
            UIField *focusField = GetFocus();
            if (focusField == loadField_) {
                // Open the appropriate plugin browser based on current type
                if (current_) {
                    if (current_->GetEffectType() == ET_VST3) {
                        ImportVST3EffectDialog *dialog = new ImportVST3EffectDialog(*this, current_);
                        DoModal(dialog, EffectPluginSelectCallback);
                    } else {
                        LV2Effect *lv2fx = dynamic_cast<LV2Effect*>(current_);
                        if (lv2fx) {
                            ImportLV2EffectDialog *dialog = new ImportLV2EffectDialog(*this, lv2fx);
                            DoModal(dialog, EffectPluginSelectCallback);
                        }
                    }
                }
                return;
            }
        }
        if (mask == EPBM_A) {
            mask &= (0xFFFF - (EPBM_A | EPBM_L));
        }
    }

    if (viewMode_ == VM_SELECTION) {
    } else {
        viewMode_ = VM_NORMAL;
    }

    // B modifier: navigate between effect slots
    if (mask & EPBM_B) {
        if (mask & EPBM_LEFT) warpToNext(-1);
        else if (mask & EPBM_RIGHT) warpToNext(+1);
        else if (mask & EPBM_DOWN) warpToNext(-16);
        else if (mask & EPBM_UP) warpToNext(+16);
        else if (mask & EPBM_A) {
            // Purge effect
            if (current_ && !current_->IsEmpty()) {
                current_->Purge();
                onEffectChange();
                isDirty_ = true;
            }
        }
        return;
    }

    FieldView::ProcessButtonMask(mask);

    {
        if (mask == EPBM_A) {
            UIField *focusField = GetFocus();
            if (focusField == loadField_) {
                viewMode_ = VM_NEW;
            }
        } else {
            if (mask & EPBM_R) {
                if (mask & EPBM_LEFT) {
                    ViewType vt = VT_INSTRUMENT;
                    ViewEvent ve(VET_SWITCH_VIEW, &vt);
                    SetChanged();
                    NotifyObservers(&ve);
                }
                if (mask & EPBM_DOWN) {
                    ViewType vt = VT_TABLE2;
                    ViewEvent ve(VET_SWITCH_VIEW, &vt);
                    SetChanged();
                    NotifyObservers(&ve);
                }

                Player *player = Player::GetInstance();
                if (mask & EPBM_START) {
                    player->OnStartButton(PM_PHRASE, viewData_->songX_, true,
                                          viewData_->chainRow_);
                }
            } else if (mask & EPBM_L) {
                // L shoulder: scroll parameters with wrap-around
                const int PARAMS_PER_PAGE = 36;
                if (current_ && !current_->IsEmpty() && current_->GetParameterCount() > PARAMS_PER_PAGE) {
                    scrollOffset_ += 18;
                    if (scrollOffset_ >= current_->GetParameterCount()) {
                        scrollOffset_ = 0;
                    }
                    onEffectChange();
                    isDirty_ = true;
                    return;
                }
            } else {
                Player *player = Player::GetInstance();
                if (mask & EPBM_START) {
                    player->OnStartButton(PM_PHRASE, viewData_->songX_, false,
                                          viewData_->chainRow_);
                }
            }
        }
    }
}

void EffectView::DrawView() {

    // Process any pending type switch before drawing to avoid showing stale UI
    if (pendingTypeEffectIdx_ != -1) {
        int idx = pendingTypeEffectIdx_;
        EffectType type = pendingEffectType_;
        pendingTypeEffectIdx_ = -1;
        Trace::Log("EVIEW", "DrawView: processing pending type switch effect=%d type=%d", idx, (int)type);
        // Stop playback before switching type to prevent crash from
        // the audio thread calling ProcessAudio on a deleted effect
        Player *player = Player::GetInstance();
        if (player && player->IsRunning()) {
            player->Stop();
        }
        EffectBank *bank = project_->GetEffectBank();
        if (bank) {
            bank->SetEffectType(idx, type);
            current_ = bank->GetEffect(idx);
        }
        currentTypeIndex_ = (type == ET_VST3) ? 0 : 1;
        scrollOffset_ = 0;
        current_ = nullptr;
        onEffectChange();
    }

    Clear();
    View::EnableNotification();

    GUITextProperties props;
    GUIPoint pos = GetTitlePosition();

    char title[20];
    SetColor(CD_NORMAL);
    sprintf(title, "Effect %2.2X", currentEffect_);
    DrawString(pos._x, pos._y, title, props);

    FieldView::Redraw();
    drawMap();
}

void EffectView::OnFocus() {
    onEffectChange();
}

void EffectView::OnEffectPluginSelected() {
    scrollOffset_ = 0;
    onEffectChange();
    isDirty_ = true;
}

void EffectView::Update(Observable &o, I_ObservableData *d) {
    UIActionField *action = dynamic_cast<UIActionField *>(&o);
    if (action) {
        if (d) {
#ifdef _64BIT
            unsigned int fourcc = *(unsigned int *)d;
#else
            unsigned int fourcc = (unsigned int)(intptr_t)d;
#endif
            if (fourcc == ACTION_LOAD_EFFECT) {
                if (current_) {
                    if (current_->GetEffectType() == ET_VST3) {
                        ImportVST3EffectDialog *dialog = new ImportVST3EffectDialog(*this, current_);
                        DoModal(dialog, EffectPluginSelectCallback);
                    } else {
                        LV2Effect *lv2fx = dynamic_cast<LV2Effect*>(current_);
                        if (lv2fx) {
                            ImportLV2EffectDialog *dialog = new ImportLV2EffectDialog(*this, lv2fx);
                            DoModal(dialog, EffectPluginSelectCallback);
                        }
                    }
                }
                return;
            }

        }
    }

    // Check EFTP type switch FIRST — must take priority over preset changes
    if (current_) {
        Variable *tv = current_->FindVariable(EFTP);
        if (tv) {
            int val = tv->GetInt();
            EffectType targetType = (val == 0) ? ET_VST3 : ET_LV2;
            if (current_->GetEffectType() != targetType) {
                // Defer changing effect type until DrawView(), outside the
                // variable notification callback. DrawView stops the player
                // first to prevent the audio thread from using freed memory.
                pendingTypeEffectIdx_ = currentEffect_;
                pendingEffectType_ = targetType;
                Trace::Log("EVIEW", "Update: deferring type switch effect=%d from=%d to=%d", currentEffect_, (int)current_->GetEffectType(), (int)targetType);
                isDirty_ = true;
                return;
            }
        }
    }

    // Handle VST3 effect bank/preset variable changes
    if (current_ && current_->GetEffectType() == ET_VST3) {
        VST3Effect *vst3fx = (VST3Effect *)current_;
        Variable *bv = vst3fx->FindVariable(VST3FX_BANK);
        if (bv && vst3fx->GetCurrentBank() != bv->GetInt()) {
            vst3fx->SetCurrentBank(bv->GetInt());
            // Reset preset to 0 when bank changes
            Variable *pv = vst3fx->FindVariable(VST3FX_PRESET);
            if (pv) pv->SetInt(0);
            vst3fx->SetPreset(0);
            onEffectChange();
            isDirty_ = true;
            return;
        }
        Variable *pv = vst3fx->FindVariable(VST3FX_PRESET);
        if (pv && vst3fx->GetCurrentPreset() != pv->GetInt()) {
            vst3fx->SetPreset(pv->GetInt());
            onEffectChange();
            isDirty_ = true;
            return;
        }
    }

    onEffectChange();
}

