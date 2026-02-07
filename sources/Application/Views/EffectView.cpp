#include "EffectView.h"
#include "Application/Instruments/LV2Effect.h"
#include "Application/Model/Config.h"
#include "BaseClasses/UIIntVarField.h"
#include "BaseClasses/UIIntVarOffField.h"
#include "BaseClasses/UILV2EffectParameterField.h"
#include "BaseClasses/UIStaticField.h"
#include "BaseClasses/UIActionField.h"
#include "Foundation/Variables/Variable.h"
#include "Foundation/Variables/WatchedVariable.h"
#include "ModalDialogs/ImportLV2EffectDialog.h"
#include "ModalDialogs/MessageBox.h"
#include "System/Console/Trace.h"
#include <cstring>
#include <vector>
#include <string>
#include <map>

#define ACTION_LOAD_LV2_EFFECT MAKE_FOURCC('E','F','L','D')

// Callback for effect plugin selection dialog
static void EffectPluginSelectCallback(View &v, ModalView &dialog) {
    EffectView &ev = (EffectView &)v;
    ev.OnEffectPluginSelected();
}

EffectView::EffectView(GUIWindow &w, ViewData *data) : FieldView(w, data) {
    project_ = data->project_;
    currentEffect_ = 0;
    lv2ScrollOffset_ = 0;
    current_ = nullptr;
    lv2LoadField_ = nullptr;
    memset(lv2PluginLabel_, 0, sizeof(lv2PluginLabel_));
    memset(lv2ParamText_, 0, sizeof(lv2ParamText_));
}

EffectView::~EffectView() {
}

void EffectView::onEffectChange() {
    ClearFocus();

    LV2Effect *old = current_;
    current_ = project_->GetEffect(currentEffect_);
    if (!current_) return;

    if (current_ != old && old != nullptr) {
        // No observer pattern needed on effects for now
    }
    T_SimpleList<UIField>::Empty();

    fillEffectParameters();

    SetFocus(T_SimpleList<UIField>::GetFirst());
}

void EffectView::fillEffectParameters() {
    if (!current_) return;

    GUIPoint position = GetAnchor();

    // Constants for two-column layout
    const int COL_WIDTH = 27;
    const int MAX_ROWS = 17;
    const int PARAMS_PER_PAGE = MAX_ROWS * 2;

    // Display help text at top-right
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

    position._y += 1;

    // Display the plugin selector as a clickable action (opens LV2 effect browser)
    if (current_->IsEmpty()) {
        strcpy(lv2PluginLabel_, "load list");
    } else {
        snprintf(lv2PluginLabel_, sizeof(lv2PluginLabel_), "plugin: %s", current_->GetName());
    }
    UIActionField *af = new UIActionField(lv2PluginLabel_, ACTION_LOAD_LV2_EFFECT, position);
    T_SimpleList<UIField>::Insert(af);
    af->AddObserver(*this);
    lv2LoadField_ = af;

    position._y += 1;

    // Show parameters if plugin is loaded
    if (!current_->IsEmpty()) {
        int paramCount = current_->GetParameterCount();
        if (paramCount > 0) {
            // Group parameters by groupName, preserving order
            std::vector<std::pair<std::string, std::vector<int>>> groups;
            std::map<std::string, int> groupIndex;

            for (int p = 0; p < paramCount; p++) {
                const LV2PluginParameter *param = current_->GetParameter(p);
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

            // Display in two columns
            int baseY = position._y;
            int baseX = 0;
            int col = 0;
            int row = 0;
            int textIdx = 0;

            for (int i = startIdx; i < endIdx && textIdx < 40; i++, textIdx++) {
                int p = displayOrder[i];
                const LV2PluginParameter *param = current_->GetParameter(p);
                if (!param || !param->variable) continue;

                GUIPoint pos(baseX + col * COL_WIDTH, baseY + row);

                strncpy(lv2ParamText_[textIdx], param->name.c_str(), 22);
                lv2ParamText_[textIdx][22] = '\0';

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

                UILV2EffectParameterField *pfield = new UILV2EffectParameterField(
                    pos,
                    *param->variable,
                    lv2ParamText_[textIdx],
                    current_,
                    p,
                    0,    // min
                    127,  // max
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
        } else {
            position._y += 2;
        }

        // Footer controls: volume and wet/dry
        Variable *v = current_->FindVariable(LV2FX_VOLUME);
        if (v) {
            UIIntVarField *f1 = new UIIntVarField(position, *v, "volume: %2.2X", 0, 0xFF, 1, 0x10);
            T_SimpleList<UIField>::Insert(f1);
            position._y += 1;
        }

        v = current_->FindVariable(LV2FX_WETDRY);
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
    if (effect >= MAX_LV2EFFECT_COUNT) {
        effect = effect - MAX_LV2EFFECT_COUNT;
    }
    if (effect < 0) {
        effect = MAX_LV2EFFECT_COUNT + effect;
    }
    currentEffect_ = effect;
    lv2ScrollOffset_ = 0;
    onEffectChange();
    isDirty_ = true;
}

void EffectView::ProcessButtonMask(unsigned short mask, bool pressed) {
    if (!pressed) return;

    isDirty_ = false;

    // Handle A press on load field
    if (viewMode_ == VM_NEW) {
        if (mask == EPBM_A) {
            UIField *focusField = GetFocus();
            if (focusField == lv2LoadField_) {
                ImportLV2EffectDialog *dialog = new ImportLV2EffectDialog(*this, current_);
                DoModal(dialog, EffectPluginSelectCallback);
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

    FieldView::ProcessButtonMask(mask);

    // B modifier: navigate between effect slots
    if (mask & EPBM_B) {
        if (mask & EPBM_LEFT) warpToNext(-1);
        if (mask & EPBM_RIGHT) warpToNext(+1);
        if (mask & EPBM_DOWN) warpToNext(-16);
        if (mask & EPBM_UP) warpToNext(+16);
        if (mask & EPBM_A) {
            // Purge effect
            if (current_ && !current_->IsEmpty()) {
                current_->Purge();
                onEffectChange();
                isDirty_ = true;
            }
        }
    } else {
        // A modifier
        if (mask == EPBM_A) {
            UIField *focusField = GetFocus();
            if (focusField == lv2LoadField_) {
                viewMode_ = VM_NEW;
            }
        } else {
            // R modifier: navigate to other views
            if (mask & EPBM_R) {
                if (mask & EPBM_LEFT) {
                    // Go to instrument view
                    ViewType vt = VT_INSTRUMENT;
                    ViewEvent ve(VET_SWITCH_VIEW, &vt);
                    SetChanged();
                    NotifyObservers(&ve);
                }
                if (mask & EPBM_DOWN) {
                    // Go to table view
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
                // L shoulder: scroll LV2 parameters with wrap-around
                const int PARAMS_PER_PAGE = 36;
                if (current_ && !current_->IsEmpty() && current_->GetParameterCount() > PARAMS_PER_PAGE) {
                    lv2ScrollOffset_ += 18;
                    if (lv2ScrollOffset_ >= current_->GetParameterCount()) {
                        lv2ScrollOffset_ = 0;
                    }
                    onEffectChange();
                    isDirty_ = true;
                    return;
                }
            } else {
                // No modifier
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
    Clear();
    View::EnableNotification();

    GUITextProperties props;
    GUIPoint pos = GetTitlePosition();

    // Draw title
    char title[20];
    SetColor(CD_NORMAL);
    sprintf(title, "Effect %2.2X", currentEffect_);
    DrawString(pos._x, pos._y, title, props);

    // Draw fields
    FieldView::Redraw();
    drawMap();
}

void EffectView::OnFocus() {
    onEffectChange();
}

void EffectView::OnEffectPluginSelected() {
    lv2ScrollOffset_ = 0;
    onEffectChange();
    isDirty_ = true;
}

void EffectView::Update(Observable &o, I_ObservableData *d) {
    // Handle action field clicks (LV2 effect load action)
    UIActionField *action = dynamic_cast<UIActionField *>(&o);
    if (action) {
        if (d) {
#ifdef _64BIT
            unsigned int fourcc = *(unsigned int *)d;
#else
            unsigned int fourcc = (unsigned int)(intptr_t)d;
#endif
            if (fourcc == ACTION_LOAD_LV2_EFFECT) {
                if (current_) {
                    ImportLV2EffectDialog *dialog = new ImportLV2EffectDialog(*this, current_);
                    DoModal(dialog, EffectPluginSelectCallback);
                }
                return;
            }
        }
    }
    onEffectChange();
}
