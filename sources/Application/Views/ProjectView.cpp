#include "ProjectView.h"
#include "Application/Mixer/MixerService.h"
#include "Application/Model/ProjectDatas.h"
#include "Application/Model/Scale.h"
#include "Application/Persistency/PersistencyService.h"
#include "Application/Views/ModalDialogs/MessageBox.h"
#include "Application/Views/ModalDialogs/NewProjectDialog.h"
#include "Application/Views/ModalDialogs/SelectProjectDialog.h"
#include "BaseClasses/UIActionField.h"
#include "BaseClasses/UIField.h"
#include "BaseClasses/UIIntVarField.h"
#include "BaseClasses/UIStaticField.h"
#include "BaseClasses/UITempoField.h"
#include "System/System/System.h"
#include "Application/Instruments/SamplePool.h"
#include <cstdint>

#define ACTION_PURGE            MAKE_FOURCC('P','U','R','G')
#define ACTION_SAVE             MAKE_FOURCC('S','A','V','E')
#define ACTION_SAVE_AS          MAKE_FOURCC('S','V','A','S')
#define ACTION_LOAD             MAKE_FOURCC('L','O','A','D')
#define ACTION_QUIT             MAKE_FOURCC('Q','U','I','T')
#define ACTION_PURGE_INSTRUMENT MAKE_FOURCC('P','R','G','I')
#define ACTION_TEMPO_CHANGED    MAKE_FOURCC('T','E','M','P')
#define ACTION_INSTALL_SAMPLES  MAKE_FOURCC('I','N','S','T')
#define ACTION_INSTALL_LV2      MAKE_FOURCC('I','L','V','2')
#define ACTION_THEME            MAKE_FOURCC('T','H','M','E')

static void SaveAsProjectCallback(View &v,ModalView &dialog) {

    FileSystemService FSS;
    NewProjectDialog &npd=(NewProjectDialog &)dialog;

    if (dialog.GetReturnCode()>0) {
        std::string str_dstprjdir;
        std::string str_dstsmpdir;

        Path root("root:");
        // place new projects under the projects folder in the root SDTracker dir
        str_dstprjdir = root.GetName() + "/projects/" + npd.GetName();
        str_dstsmpdir = str_dstprjdir + "/samples/";

		Path path_srcprjdir("project:");
		Path path_srcsmpdir("project:samples");
		Path path_dstprjdir = Path(str_dstprjdir);
		Path path_dstsmpdir = Path(str_dstsmpdir);

        Path path_srclgptdatsav = path_srcprjdir.GetPath() + "lgptsav_tmp.dat";
        Path path_dstlgptdatsav = path_dstprjdir.GetPath() + "/lgptsav.dat";

		if (path_dstprjdir.Exists()) {
			Trace::Log("ProjectView", "Dst Dir '%s' Exist == true",
			path_dstprjdir.GetPath().c_str());
		} else {
			if (FileSystem::GetInstance()->MakeDir(path_dstprjdir.GetPath().c_str()).Failed()) {
				Trace::Log("ProjectView", "Failed to create dir '%s'", path_dstprjdir.GetPath().c_str());
				return;
			};

		if (FileSystem::GetInstance()->MakeDir(path_dstsmpdir.GetPath().c_str()).Failed()) {
			Trace::Log("ProjectView", "Failed to create sample dir '%s'", path_dstprjdir.GetPath().c_str());
			return;
		};

        if (FSS.Copy(path_srclgptdatsav, path_dstlgptdatsav) > -1) {
            FSS.Delete(path_srclgptdatsav);
        }

        I_Dir *idir_srcsmpdir =
            FileSystem::GetInstance()->Open(path_srcsmpdir.GetPath().c_str());
        if (idir_srcsmpdir) {
				idir_srcsmpdir->GetContent("*");
				idir_srcsmpdir->Sort();
				IteratorPtr<Path>it(idir_srcsmpdir->GetIterator());
				for (it->Begin();!it->IsDone();it->Next()) {
					Path &current=it->CurrentItem();
					if (current.IsFile()) {
						Path dstfile = Path((str_dstsmpdir+current.GetName()).c_str());
						Path srcfile = Path(current.GetPath());
						FSS.Copy(srcfile.GetPath(),dstfile.GetPath());
					}
				}
			}

		((ProjectView &)v).OnSaveAsProject((char*)str_dstprjdir.c_str());
		}
    }
}

static void LoadCallback(View &v,ModalView &dialog) {
    MixerService::GetInstance()->SetRenderMode(0);
    if (dialog.GetReturnCode()==MBL_YES) {
		((ProjectView &)v).OnLoadProject() ;
	}
} ;

static void QuitCallback(View &v,ModalView &dialog) {
    MixerService::GetInstance()->SetRenderMode(0);
    if (dialog.GetReturnCode()==MBL_YES) {
		((ProjectView &)v).OnQuit() ;
	}
} ;

static void PurgeCallback(View &v,ModalView &dialog) {
	((ProjectView &)v).OnPurgeInstruments(dialog.GetReturnCode()==MBL_YES) ;
} ;

// Callback for installing default samples
static void InstallSamplesCallback(View &v,ModalView &dialog) {
    if (dialog.GetReturnCode()!=MBL_YES) return;

    ProjectView &pv = (ProjectView &)v;

    // Download samplelib.zip from the release URL into a temp file
    const char *url = "https://github.com/PatttF/SteamDeckTracker/releases/download/Release/samplelib.zip";
    std::string tmp = "/tmp/samplelib.zip";
    {
        // Try curl first, fallback to wget
        std::string cmd = std::string("curl -fL -o \"") + tmp + "\" \"" + std::string(url) + "\"";
        int rc = system(cmd.c_str());
        if (rc != 0) {
            cmd = std::string("wget -O \"") + tmp + "\" \"" + std::string(url) + "\"";
            rc = system(cmd.c_str());
            if (rc != 0) {
                MessageBox *mb = new MessageBox(pv, "Failed to download samplelib.zip from release URL", MBBF_OK);
                pv.DoModal(mb);
                return;
            }
        }
    }

    Path zipPath(tmp.c_str());
    if (!zipPath.Exists()) {
        MessageBox *mb = new MessageBox(pv, "Downloaded zip not found", MBBF_OK);
        pv.DoModal(mb);
        return;
    }

    // Ensure destination exists
    const char *destStr = SamplePool::GetInstance()->GetSampleLib();
    Path dest(destStr);
    if (FileSystem::GetInstance()->GetFileType(dest.GetPath().c_str()) != FT_DIR) {
        if (FileSystem::GetInstance()->MakeDir(dest.GetPath().c_str()).Failed()) {
            MessageBox *mb = new MessageBox(pv, "Failed to create samplelib folder", MBBF_OK);
            pv.DoModal(mb);
            return;
        }
    }

    // Use system unzip if available. Use canonical paths so they are real FS paths.
    std::string zipCanon = zipPath.GetCanonicalPath();
    std::string destCanon = dest.GetCanonicalPath();

    std::string cmd = "unzip -o \"" + zipCanon + "\" -d \"" + destCanon + "\"";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        MessageBox *mb = new MessageBox(pv, "Failed to unpack samplelib.zip (unzip returned error)", MBBF_OK);
        pv.DoModal(mb);
        return;
    }

    // Reload sample pool
    SamplePool::GetInstance()->Reset();
    SamplePool::GetInstance()->Load();

    pv.SetNotification("Default samples installed");
} ;

// Callback for installing LV2 plugins from GitHub
static void InstallLV2Callback(View &v,ModalView &dialog) {
    if (dialog.GetReturnCode()!=MBL_YES) return;

    ProjectView &pv = (ProjectView &)v;

    // Create ~/.lv2 directory if it doesn't exist
    const char* homeDir = getenv("HOME");
    if (!homeDir) {
        MessageBox *mb = new MessageBox(pv, "Could not determine home directory", MBBF_OK);
        pv.DoModal(mb);
        return;
    }
    
    char lv2Dir[512];
    snprintf(lv2Dir, sizeof(lv2Dir), "%s/.lv2", homeDir);
    
    char createDirCmd[1024];
    snprintf(createDirCmd, sizeof(createDirCmd), "mkdir -p '%s'", lv2Dir);
    system(createDirCmd);
    
    // Download and install plugins
    char downloadCmd[2048];
    snprintf(downloadCmd, sizeof(downloadCmd), 
        "cd /tmp && "
        "wget -O Plugins.zip 'https://github.com/PatttF/SteamDeckTracker/releases/download/0.2.1/Plugins.zip' && "
        "unzip -o Plugins.zip -d '%s' && "
        "chmod -R 755 '%s'/*.lv2 && "
        "rm -f Plugins.zip", 
        lv2Dir, lv2Dir);
    
    int result = system(downloadCmd);
    
    if (result == 0) {
        pv.SetNotification("LV2 plugins installed successfully");
    } else {
        MessageBox *mb = new MessageBox(pv, "Failed to install LV2 plugins", MBBF_OK);
        pv.DoModal(mb);
    }
}

ProjectView::ProjectView(GUIWindow &w,ViewData *data):FieldView(w,data) {

    lastClock_ = 0;
    lastTick_ = 0;

	project_=data->project_ ;

	GUIPoint position=GetAnchor() ;
	
	Variable *v=project_->FindVariable(VAR_TEMPO) ;
    UITempoField *f = new UITempoField(ACTION_TEMPO_CHANGED, position, *v,
                                       "Tempo: %d [%2.2x]  ", 60, 400, 1, 10);
    T_SimpleList<UIField>::Insert(f) ;
	f->AddObserver(*this) ;
	tempoField_=f ;

    v = project_->FindVariable(VAR_MASTERVOL);
    position._y += 1;
    UIIntVarField *field =
        new UIIntVarField(position, *v, "Master: %d", 10, 100, 1, 10);
    T_SimpleList<UIField>::Insert(field);

    v = project_->FindVariable(VAR_PREGAIN);
    position._y += 2;
    field = new UIIntVarField(position, *v, "Drive: %d", 10, 200, 1, 10);
    T_SimpleList<UIField>::Insert(field);

    position._y += 1;
    v = project_->FindVariable(VAR_SOFTCLIP);
    field = new UIIntVarField(position, *v, "Type: %s", 0, 4, 1, 4);
    T_SimpleList<UIField>::Insert(field);

    v = project_->FindVariable(VAR_SOFTCLIP_GAIN);
    position._x += 13;
    field = new UIIntVarField(position, *v, "%s", 0, 1, 1, 1);
    T_SimpleList<UIField>::Insert(field);
    position._x -= 13;

    v = project_->FindVariable(VAR_TRANSPOSE);
    position._y += 2;
    UIIntVarField *f2=new UIIntVarField(position,*v,"Transpose: %3.2d",-48,48,0x1,0xC) ;
	T_SimpleList<UIField>::Insert(f2) ;

    v = project_->FindVariable(VAR_SCALE);
	// if scale name is not found, set the default chromatic scale
	if (v->GetInt() < 0) {
		v->SetInt(0);
    }
    position._y += 1;
    field =
        new UIIntVarField(position, *v, "Scale: %s", 0, scaleCount - 1, 1, 10);
    T_SimpleList<UIField>::Insert(field);

    position._y += 2;
    UIActionField *a1 =
        new UIActionField("Compact Sequencer", ACTION_PURGE, position);
    a1->AddObserver(*this);
    T_SimpleList<UIField>::Insert(a1);

    position._y += 1;
    a1 = new UIActionField("Compact Instruments", ACTION_PURGE_INSTRUMENT,
                           position);
    a1->AddObserver(*this);
    T_SimpleList<UIField>::Insert(a1);

    position._y += 2;
    a1 = new UIActionField("Load Song", ACTION_LOAD, position);
    a1->AddObserver(*this);
    T_SimpleList<UIField>::Insert(a1);

    position._y += 1;
    a1 = new UIActionField("Save Song", ACTION_SAVE, position);
    a1->AddObserver(*this);
    T_SimpleList<UIField>::Insert(a1);

    position._y += 1;
    a1 = new UIActionField("Save Song As", ACTION_SAVE_AS, position);
    a1->AddObserver(*this);
    T_SimpleList<UIField>::Insert(a1);

    position._y += 1;
    a1 = new UIActionField("Install default samples", ACTION_INSTALL_SAMPLES, position);
    a1->AddObserver(*this);
    T_SimpleList<UIField>::Insert(a1);

    position._y += 1;
    a1 = new UIActionField("Install plugins", ACTION_INSTALL_LV2, position);
    a1->AddObserver(*this);
    T_SimpleList<UIField>::Insert(a1);

    position._y += 1;
    a1 = new UIActionField("Theme", ACTION_THEME, position);
    a1->AddObserver(*this);
    T_SimpleList<UIField>::Insert(a1);

    position._y += 2;
    v = project_->FindVariable(VAR_RENDER);
    NAssert(v);
    field = new UIIntVarField(position, *v, "Render: %s", 0,
                              project_->MAX_RENDER_MODE - 1, 1, 2);
    T_SimpleList<UIField>::Insert(field);

    position._y += 2;
    a1 = new UIActionField("Exit", ACTION_QUIT, position);
    a1->AddObserver(*this);
    T_SimpleList<UIField>::Insert(a1);

}

ProjectView::~ProjectView() {
}

void ProjectView::ProcessButtonMask(unsigned short mask,bool pressed) {

    if (!pressed)
        return;

    FieldView::ProcessButtonMask(mask);

    if (mask & EPBM_R) {
        if (mask&EPBM_DOWN) {
			ViewType vt=VT_SONG;
			ViewEvent ve(VET_SWITCH_VIEW,&vt) ;
			SetChanged();
            NotifyObservers(&ve);
        }
    } else {
        if (mask&EPBM_START) {
            Player *player = Player::GetInstance();

            int renderMode = viewData_->renderMode_;
			if (renderMode > 0 && !player->IsRunning()) {
				viewData_->isRendering_ = true;
				View::SetNotification("Rendering started!");
			} else if (viewData_->isRendering_ && player->IsRunning()) {
				viewData_->isRendering_ = false;
				View::SetNotification("Rendering done!");
			}

			player->OnStartButton(PM_SONG,viewData_->songX_,false,viewData_->songX_) ;
		}
    };
} ;

void ProjectView::DrawView() {

    Clear() ;

	GUITextProperties props ;
	GUIPoint pos=GetTitlePosition() ;

// Draw title

	// Title: show generic project label (build info removed)
	SetColor(CD_NORMAL);
	DrawString(pos._x, pos._y, "Project", props);

    FieldView::Redraw();
    drawMap();

    int currentMode = project_->GetRenderMode();
    if ((viewData_->renderMode_ != currentMode) && !MixerService::GetInstance()->IsRendering()) {
        // Mode changed
        if (currentMode > 0 && viewData_->renderMode_ == 0) {
            View::SetNotification("Rendering on, press start");
        } else if (currentMode == 0 && viewData_->renderMode_ > 0) {
            View::SetNotification("Rendering off");
        }
        viewData_->renderMode_ = currentMode;
        MixerService::GetInstance()->SetRenderMode(currentMode);
    }

    View::EnableNotification();
} ;

void ProjectView::Update(Observable &,I_ObservableData *data) {

	if (!hasFocus_) {
		return ;
	}

#ifdef _64BIT
    int fourcc = *((unsigned int *)data);
#else
    int fourcc = (int)(intptr_t)data;
#endif

    UIField *focus = GetFocus();
    if (fourcc!= ACTION_TEMPO_CHANGED) {
		focus->ClearFocus() ;
		focus->Draw(w_) ;
		w_.Flush() ;
		focus->SetFocus() ;
	} else {
		focus=tempoField_ ;
	}
    Player *player = Player::GetInstance();
    switch (fourcc) {
		case ACTION_PURGE:
			project_->Purge() ;
			break ;
		case ACTION_PURGE_INSTRUMENT:
		{
			MessageBox *mb=new MessageBox(*this,"Purge unused samples from disk ?",MBBF_YES|MBBF_NO) ;
			DoModal(mb,PurgeCallback) ;
			break ;
		}
        case ACTION_SAVE: {
            MixerService::GetInstance()->SetRenderMode(0);
            PersistencyService *service = PersistencyService::GetInstance();
            service->Save();
            break;
        }
        case ACTION_SAVE_AS: {
            PersistencyService *service = PersistencyService::GetInstance();
            service->Save("project:lgptsav_tmp.dat");
            // start the New Project dialog inside the projects folder
            NewProjectDialog *mb = new NewProjectDialog(*this, "root:projects");
            DoModal(mb, SaveAsProjectCallback);
            break;
        }
        case ACTION_LOAD: {
            MessageBox *mb = new MessageBox(
                *this, "Load song and lose changes ?", MBBF_YES | MBBF_NO);
            DoModal(mb, LoadCallback);
            break;
        }
        case ACTION_INSTALL_SAMPLES: {
            MessageBox *mb = new MessageBox(*this, "Install default samples into samplelib?", MBBF_YES | MBBF_NO);
            DoModal(mb, InstallSamplesCallback);
            break;
        }
        case ACTION_INSTALL_LV2: {
            MessageBox *mb = new MessageBox(*this, "Install LV2 packages on this system? (requires sudo)", MBBF_YES | MBBF_NO);
            DoModal(mb, InstallLV2Callback);
            break;
        }
        case ACTION_THEME: {
            ViewType vt = VT_THEME;
            ViewEvent ve(VET_SWITCH_VIEW, &vt);
            SetChanged();
            NotifyObservers(&ve);
            break;
        }
        case ACTION_QUIT: {
            MessageBox *mb = new MessageBox(*this, "Quit and lose faith ?",
                                            MBBF_YES | MBBF_NO);
            DoModal(mb, QuitCallback);
            break;
        }
        case ACTION_TEMPO_CHANGED:
			break ;
		default:
			NInvalid ;
			break ;
	} ;
    focus->Draw(w_) ;
	isDirty_=true ;
} ;

void ProjectView::OnPurgeInstruments(bool removeFromDisk) {
	project_->PurgeInstruments(removeFromDisk) ;
} ;

void ProjectView::OnLoadProject() {
	ViewEvent ve(VET_QUIT_PROJECT) ;
	SetChanged();
	NotifyObservers(&ve) ;
} ;

void ProjectView::OnSaveAsProject(char * data) {
        ViewEvent ve(VET_SAVEAS_PROJECT,data) ;
	SetChanged();
	NotifyObservers(&ve) ;
} ;

void ProjectView::OnQuit() {
	ViewEvent ve(VET_QUIT_APP) ;
	SetChanged();
	NotifyObservers(&ve) ;
} ;
