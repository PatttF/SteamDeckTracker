#include "AppWindow.h"
#include "Application/Commands/ApplicationCommandDispatcher.h"
#include "Application/Commands/EventDispatcher.h"
#include "Application/Instruments/SamplePool.h"
#include "Application/Mixer/MixerService.h"
#include "Application/Model/Song.h"
#include "Application/Persistency/PersistencyService.h"
#include "Application/Player/TablePlayback.h"
#include "Application/Utils/char.h"
#include "Application/Views/ModalDialogs/MessageBox.h"
#include "Application/Views/ModalDialogs/SelectProjectDialog.h"
#include "Foundation/Variables/WatchedVariable.h"
#include "Player/Player.h"
#include "Services/Midi/MidiService.h"
#include "System/Console/Trace.h"
#include "UIFramework/Interfaces/I_GUIWindowFactory.h"
#include "Views/UIController.h"
#include <string.h>
#include "Adapters/SDL2/GUI/SDLGUIWindowImp.h"

AppWindow *instance = 0;

GUIColor AppWindow::backgroundColor_(0x1D, 0x0A, 0x1F);
GUIColor AppWindow::normalColor_(0xF5, 0xEB, 0xFF);
GUIColor AppWindow::borderColor_(0xFF, 0x00, 0x8C);
GUIColor AppWindow::clipColor_(0xFF, 0x00, 0x00);
GUIColor AppWindow::songviewfeColor_(0xA5, 0x5B, 0x8F);
GUIColor AppWindow::songview00Color_(0x85, 0x3B, 0x6F);
GUIColor AppWindow::highlightColor_(0xB7, 0x50, 0xD1);
GUIColor AppWindow::highlight2Color_(0xDB, 0x33, 0xDB);
GUIColor AppWindow::consoleColor_(0x00, 0xFF, 0x00);
GUIColor AppWindow::cursorColor_(0xFF, 0x00, 0x8C);
GUIColor AppWindow::playColor_(0xFF, 0x00, 0x8C);
GUIColor AppWindow::muteColor_(0xF5, 0xEB, 0xFF);
GUIColor AppWindow::rownumberColor_(0xBA, 0x28, 0xF9);
GUIColor AppWindow::rownumber2Color_(0xFF, 0x00, 0xFF);
GUIColor AppWindow::majorbeatColor_(0xBA, 0x28, 0xF9);

int AppWindow::charWidth_ = 8;
int AppWindow::charHeight_ = 8;

// #define _FORCE_SDL_EVENT_

static void ProjectSelectCallback(View &v, ModalView &dialog) {

    SelectProjectDialog &spd = (SelectProjectDialog &)dialog;
    if (dialog.GetReturnCode() > 0) {
        Path selected = spd.GetSelection();
        instance->SaveLastProject(selected);
        instance->LoadProject(selected.GetPath().c_str());
    } else {
        System::GetInstance()->PostQuitMessage();
    }
};

void AppWindow::defineColor(const char *colorName, GUIColor &color) {

    Config *config = Config::GetInstance();
    const char *value = config->GetValue(colorName);
    if (value) {
        unsigned char r;
        char2hex(value, &r);
        unsigned char g;
        char2hex(value + 2, &g);
        unsigned char b;
        char2hex(value + 4, &b);
        color = GUIColor(r, g, b);
    }
}

AppWindow::AppWindow(I_GUIWindowImp &imp) : GUIWindow(imp) {

    instance = this;

    // Init all members

    _statusLine[0] = 0;

    _currentView = 0;
    _viewData = 0;
    _songView = 0;
    _chainView = 0;
    _phraseView = 0;
    _projectView = 0;
    _instrumentView = 0;
    _effectView = 0;
    _tableView = 0;
    _nullView = 0;
    _mixerView = 0;
    _grooveView = 0;
    _themeView = 0;
    _closeProject = 0;
    _loadAfterSaveAsProject = 0;
    _loadAfterResume = 0;
    _lastA = 0;
    _lastB = 0;
    _mask = 0;
    colorIndex_ = CD_NORMAL;

    EventDispatcher *ed = EventDispatcher::GetInstance();
    ed->SetWindow(this);

    Status::Install(this);

    // Init midi services
    MidiService::GetInstance()->Init();

    defineColor("BACKGROUND", backgroundColor_);
    defineColor("FOREGROUND", normalColor_);
    defineColor("BORDER", borderColor_);
    defineColor("CLIPCOLOR", clipColor_);
    defineColor("SONGVIEW_FE", songviewfeColor_);
    defineColor("SONGVIEW_00", songview00Color_);
    defineColor("HICOLOR1", highlightColor_);
    defineColor("HICOLOR2", highlight2Color_);
    defineColor("CURSORCOLOR", cursorColor_);
    defineColor("PLAYCOLOR", playColor_);
    defineColor("MUTECOLOR", muteColor_);
    defineColor("ROWCOLOR1", rownumberColor_);
    defineColor("ROWCOLOR2", rownumber2Color_);
    defineColor("MAJORBEAT", majorbeatColor_);

    GUIWindow::Clear(backgroundColor_);

    _nullView = new NullView((*this), 0);
    _currentView = _nullView;
    _nullView->SetDirty(true);

    Config *config = Config::GetInstance(); // Possible to disable autoloading
    const char *autoLoadEnabled = config->GetValue("AUTO_LOAD_LAST");
    bool shouldAutoLoad =
        (!autoLoadEnabled || // Default to yes if not in config
         strcmp(autoLoadEnabled, "YES") == 0);

    SelectProjectDialog *spd = new SelectProjectDialog(*_currentView);
    Path lastProjectPath = GetLastProjectPath();
    if (shouldAutoLoad && lastProjectPath.Exists()) {
        
        _newProjectToLoad = lastProjectPath.GetPath().c_str();
        _loadAfterResume = true;
        delete spd;
    } else { // Show project selection dialog
        _currentView->DoModal(spd, ProjectSelectCallback);
    }

    memset(_charScreen, ' ', LOGICAL_SIZE);
    memset(_preScreen, ' ', LOGICAL_SIZE);
    memset(_charScreenProp, 0, LOGICAL_SIZE);
    memset(_preScreenProp, 0, LOGICAL_SIZE);

    Redraw();
};

AppWindow::~AppWindow() { MidiService::GetInstance()->Close(); }

void AppWindow::DrawString(const char *string, GUIPoint &pos,
                           GUITextProperties &props, bool force) {

    // we know we don't have more than LOGICAL_COLS chars

    char buffer[LOGICAL_COLS + 1];
    int len = strlen(string);
    // Reject positions that are completely out of the text buffer
    if (pos._x >= LOGICAL_COLS || pos._y >= LOGICAL_ROWS || pos._y < 0) {
        return;
    }

    int offset = 0;
    if (pos._x < 0) {
        // negative x is not supported for screen buffer; treat as fully out
        return;
    }

    // clamp length to available columns
    int available = LOGICAL_COLS - pos._x;
    len = MIN(len, available);
    memcpy(buffer, string + offset, len);
    buffer[len] = 0;
    int index = pos._x + LOGICAL_COLS * pos._y;
    if (index < 0 || index >= LOGICAL_SIZE) return;
    memcpy(_charScreen + index, buffer, len);
    unsigned char prop = colorIndex_ + (props.invert_ ? PROP_INVERT : 0);
    memset(_charScreenProp + index, prop, len);
};

void AppWindow::Clear(bool all) {
    memset(_charScreen, ' ', LOGICAL_SIZE);
    memset(_charScreenProp, 0, LOGICAL_SIZE);
    if (all) {
        memset(_preScreen, ' ', LOGICAL_SIZE);
        memset(_preScreenProp, 0, LOGICAL_SIZE);
    };
};

void AppWindow::ClearRect(GUIRect &r) {

    int x = r.Left();
    int y = r.Top();
    int w = r.Width();
    int h = r.Height();

    unsigned char *st = _charScreen + x + (LOGICAL_COLS * y);
    unsigned char *pr = _charScreenProp + x + (LOGICAL_COLS * y);
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            *st++ = ' ';
            *pr++ = 0;
        }
        st += (LOGICAL_COLS - w);
        pr += (LOGICAL_COLS - w);
    }
};

//
// Redraws the screen and flush it.
//

void AppWindow::Redraw() {

    SysMutexLocker locker(drawMutex_);

    if (_currentView) {
        _currentView->Redraw();
        Invalidate();
    }
};

//
// Flush current screen to display
//

void AppWindow::Flush() {

    // Scope the mutex to cover only the rendering work (char drawing,
    // texture upload, DrawGraphics). Present() and frame-rate limiting
    // happen OUTSIDE the lock so the audio thread's Player observer
    // updates (which also need drawMutex_) are never starved.
    {
        SysMutexLocker locker(drawMutex_);

        Lock();
        long flushStart = System::GetInstance()->GetClock();

        GUITextProperties props;
        GUIPoint pos;

        ColorDefinition color = (ColorDefinition)-1;
        pos._x = 0;
        pos._y = 0;

        int count = 0;

        unsigned char *current = _charScreen;
        unsigned char *previous = _preScreen;
        unsigned char *currentProp = _charScreenProp;
        unsigned char *previousProp = _preScreenProp;
        for (int y = 0; y < LOGICAL_ROWS; y++) {
            for (int x = 0; x < LOGICAL_COLS; x++) {
#ifndef _LGPT_NO_SCREEN_CACHE_
                if ((*current != *previous) || (*currentProp != *previousProp)) {
#endif
                    props.invert_ = (*currentProp & PROP_INVERT) != 0;
                    if (((*currentProp) & 0x7F) != color) {
                        color = (ColorDefinition)((*currentProp) & 0x7F);
                        GUIColor gcolor = normalColor_;
                        switch (color) {
                        case CD_BACKGROUND:
                            gcolor = backgroundColor_;
                            break;
                        case CD_NORMAL:
                            break;
                        case CD_BORDER:
                            gcolor = borderColor_;
                            break;
                        case CD_CLIP:
                            gcolor = clipColor_;
                            break;
                        case CD_HILITE1:
                            gcolor = highlightColor_;
                            break;
                        case CD_HILITE2:
                            gcolor = highlight2Color_;
                            break;
                        case CD_CONSOLE:
                            gcolor = consoleColor_;
                            break;
                        case CD_CURSOR:
                            gcolor = cursorColor_;
                            break;
                        case CD_PLAY:
                            gcolor = playColor_;
                            break;
                        case CD_MUTE:
                            gcolor = muteColor_;
                            break;
                        case CD_SONGVIEWFE:
                            gcolor = songviewfeColor_;
                            break;
                        case CD_SONGVIEW00:
                            gcolor = songview00Color_;
                            break;
                        case CD_ROW:
                            gcolor = rownumberColor_;
                            break;
                        case CD_ROW2:
                            gcolor = rownumber2Color_;
                            break;
                        case CD_MAJORBEAT:
                            gcolor = majorbeatColor_;
                            break;
                        default:
                            NAssert(0);
                            break;
                        }
                        GUIWindow::SetColor(gcolor);
                    }
                    GUIWindow::DrawChar(*current, pos, props);
                    count++;
#ifndef _LGPT_NO_SCREEN_CACHE_
                }
#endif
                current++;
                previous++;
                currentProp++;
                previousProp++;
                pos._x += AppWindow::charWidth_;
            }
            pos._y += AppWindow::charHeight_;
            pos._x = 0;
        }
        long flushEnd = System::GetInstance()->GetClock();

        // Flush uploads the software surface to the GPU texture and renders it
        GUIWindow::Flush();

        // Draw hardware-accelerated pixel graphics on top of the rendered texture
        if (_currentView) {
            _currentView->DrawGraphics();
            _currentView->ForwardDrawGraphicsToModal();
        }

        Unlock();
        memcpy(_preScreen, _charScreen, LOGICAL_SIZE);
        memcpy(_preScreenProp, _charScreenProp, LOGICAL_SIZE);
    }
    // drawMutex_ is now released — Present and frame pacing cannot starve
    // the audio thread.

    // Present the final composited frame (outside mutex)
    SDLGUIWindowImp *sdlImp = (SDLGUIWindowImp *)GetImpWindow();
    if (sdlImp) sdlImp->Present();

    // Manual frame-rate cap (~60 fps) to avoid burning CPU now that
    // VSYNC is disabled. This sleep happens outside all locks.
    SDL_Delay(1);
};

void AppWindow::LoadProject(const Path &p) {
    
    _root = p;

    _closeProject = false;
    _loadAfterSaveAsProject = false;

    PersistencyService *persist = PersistencyService::GetInstance();

    TablePlayback::Reset();

    // Reset all mixer channels and master to 100 before loading project values
    // This fixes edge case where changed mixer values persist when loading 
    // projects that don't contain mixer data
    MixerService *mixerService = MixerService::GetInstance();
    if (mixerService) {
        mixerService->SetMasterVolume(100);
        for (int i = 0; i < SONG_CHANNEL_COUNT; i++) {
            mixerService->SetChannelVolume(i, 100);
        }
    }

    Path::SetAlias("project", _root.GetPath().c_str());
    Path::SetAlias("samples", "project:samples");

    // Load the sample pool

    SamplePool *pool = SamplePool::GetInstance();

    pool->Load();

    Project *project = new Project();

    bool succeeded = persist->Load();
    if (!succeeded) {
        project->GetInstrumentBank()->AssignDefaults();
    };

    // Project

    WatchedVariable::Disable();

    project->GetInstrumentBank()->Init();

    WatchedVariable::Enable();

    ApplicationCommandDispatcher::GetInstance()->Init(project);

    // Create view data

    _viewData = new ViewData(project);

    // Create & observe the player
    Player *player = Player::GetInstance();
    bool playerOK = player->Init(project, _viewData);
    player->AddObserver(*this);

    // Create the controller
    UIController *controller = UIController::GetInstance();
    controller->Init(project, _viewData);

    // Create & observe all views
    _songView = new SongView((*this), _viewData, _root.GetName().c_str());
    _songView->AddObserver((*this));

    _chainView = new ChainView((*this), _viewData);
    _chainView->AddObserver((*this));

    _phraseView = new PhraseView((*this), _viewData);
    _phraseView->AddObserver((*this));

    _projectView = new ProjectView((*this), _viewData);
    _projectView->AddObserver((*this));

    _instrumentView = new InstrumentView((*this), _viewData);
    _instrumentView->AddObserver((*this));

    _effectView = new EffectView((*this), _viewData);
    _effectView->AddObserver((*this));

    _tableView = new TableView((*this), _viewData);
    _tableView->AddObserver((*this));

    _grooveView = new GrooveView((*this), _viewData);
    _grooveView->AddObserver(*this);

    _mixerView = new MixerView((*this), _viewData);
    _mixerView->AddObserver(*this);

    _sampleEditorView = new SampleEditorView((*this), _viewData);
    _sampleEditorView->AddObserver(*this);

    _themeView = new ThemeView((*this), _viewData);
    _themeView->AddObserver(*this);

    _currentView = _songView;
    _currentView->OnFocus();

    if (!playerOK) {
        MessageBox *mb =
            new MessageBox(*_songView, "Failed to initialize audio", MBBF_OK);
        _songView->DoModal(mb);
    }

    Redraw();
}

void AppWindow::CloseProject() {

    _closeProject = false;
    Player *player = Player::GetInstance();
    player->Stop();
    player->RemoveObserver(*this);

    player->Reset();

    SamplePool *pool = SamplePool::GetInstance();
    pool->Reset();

    TableHolder::GetInstance()->Reset();
    TablePlayback::Reset();

    ApplicationCommandDispatcher::GetInstance()->Close();

    SAFE_DELETE(_songView);
    SAFE_DELETE(_chainView);
    SAFE_DELETE(_phraseView);
    SAFE_DELETE(_projectView);
    SAFE_DELETE(_instrumentView);
    SAFE_DELETE(_effectView);
    SAFE_DELETE(_tableView);
    SAFE_DELETE(_sampleEditorView);
    SAFE_DELETE(_themeView);

    UIController *controller = UIController::GetInstance();
    controller->Reset();

    SAFE_DELETE(_viewData);

    _currentView = _nullView;
    _nullView->SetDirty(true);

    SelectProjectDialog *spd = new SelectProjectDialog(*_currentView);
    _currentView->DoModal(spd, ProjectSelectCallback);
};

AppWindow *AppWindow::Create(GUICreateWindowParams &params) {
    I_GUIWindowImp &imp =
        I_GUIWindowFactory::GetInstance()->CreateWindowImp(params);
    AppWindow *w = new AppWindow(imp);
    return w;
};

void AppWindow::SetDirty() { _isDirty = true; };

bool AppWindow::onEvent(GUIEvent &event) {

    // We need to tell the app to quit once we're out of the
    // mixer lock, otherwise the windows driver will never return

    _shouldQuit = false;

    _isDirty = false;

    // Handle lightweight events that don't touch audio/player state
    // BEFORE acquiring sync_ so they never contend with the audio thread.
    if (event.GetType() == ET_PLAYERUPDATE) {
        _isDirty = true;
        // Skip the sync_ lock entirely — just trigger a redraw
        goto redraw;
    }

    {
    unsigned short v = 1 << event.GetValue();

    MixerService *sm = MixerService::GetInstance();
    sm->Lock();

    switch (event.GetType()) {

    case ET_PADBUTTONDOWN:

        _mask |= v;
        if (_currentView)
            _currentView->ProcessButton(_mask, true);
        break;

    case ET_PADBUTTONUP:

        _mask &= (0xFFFF - v);
        if (_currentView)
            _currentView->ProcessButton(_mask, false);
        break;

    case ET_SYSQUIT:
        _shouldQuit = true;
        break;

        /*		case ET_KEYDOWN:
            if
           (event.GetValue()==EKT_ESCAPE&&!Player::GetInstance()->IsRunning()) {
                if (_currentView!=_listView) {
                    CloseProject() ;
                    _isDirty=true ;
                } else {
                    System::GetInstance()->PostQuitMessage() ;
                };
            } ;*/

    default:
        break;
    }
    sm->Unlock();
    }

    if (_shouldQuit) {
        onQuitApp();
    }
    if (_closeProject) {
        CloseProject();
        _isDirty = true;
    }
    if (_loadAfterSaveAsProject) {
        CloseProject();
        _isDirty = true;
        LoadProject(_newProjectToLoad.c_str());
    }
redraw:
#ifdef _SHOW_GP2X_
    Redraw();
#else
    if (_isDirty)
        Redraw();
#endif
    return false;
};

void AppWindow::onUpdate() {
    if (_loadAfterResume) {
        _loadAfterResume = false;
        _isDirty = true;
        LoadProject(_newProjectToLoad.c_str());
        return;
    }
    Flush();
};

void AppWindow::LayoutChildren() {};

void AppWindow::Update(Observable &o, I_ObservableData *d) {

    ViewEvent *ve = (ViewEvent *)d;

    switch (ve->GetType()) {

    case VET_SWITCH_VIEW: {
        ViewType *vt = (ViewType *)ve->GetData();
        
        if (_currentView) {
            
            _currentView->LooseFocus();
        }
        switch (*vt) {
        case VT_SONG:
            _currentView = _songView;
            break;
        case VT_CHAIN:
            _currentView = _chainView;
            break;
        case VT_PHRASE:
            _currentView = _phraseView;
            break;
        case VT_PROJECT:
            _currentView = _projectView;
            break;
        case VT_INSTRUMENT:
            _currentView = _instrumentView;
            break;
        case VT_EFFECT:
            _currentView = _effectView;
            break;
        case VT_TABLE:
            _currentView = _tableView;
            break;
        case VT_TABLE2:
            _currentView = _tableView;
            break;
        case VT_GROOVE:
            _currentView = _grooveView;
            break;
        case VT_MIXER:
            _currentView = _mixerView;
            break;
        case VT_SAMPLE_EDITOR:
            _currentView = _sampleEditorView;
            break;
        case VT_THEME:
            _currentView = _themeView;
            break;
        }
        if (_currentView) {
            
            _currentView->SetFocus(*vt);
        } else {
            
        }
        _isDirty = true;
        GUIWindow::Clear(backgroundColor_, true);
        Clear(true);
        Redraw();
        break;
    }

    case VET_PLAYER_POSITION_UPDATE: {
        PlayerEvent *pt = (PlayerEvent *)ve;

        if (_currentView) {
            // PET_UPDATE fires on the *audio driver thread* every buffer
            // cycle while sync_ is held.  Taking drawMutex_ here used to
            // create a priority-inversion: if the UI thread was inside
            // Redraw/Flush (holding drawMutex_), the audio thread would
            // block on drawMutex_ while still holding sync_, starving the
            // entire audio pipeline until the UI finished rendering.
            //
            // Fix: for PET_UPDATE (audio thread) just set the dirty flag.
            // The views already call OnPlayerUpdate(PET_UPDATE) at the end
            // of their own Redraw/DrawView, so play-position markers are
            // drawn on the next UI-thread Redraw cycle which is triggered
            // by the ET_PLAYERUPDATE SDL event that MixerView::Update
            // pushes into the main loop.
            //
            // PET_START / PET_STOP originate on the UI thread (Player::
            // Start/Stop called from onEvent), so taking drawMutex_ there
            // is safe — no cross-thread contention.
            if (pt->GetType() == PET_UPDATE) {
                _isDirty = true;
                SetDirty();
            } else {
                SysMutexLocker locker(drawMutex_);
                _currentView->OnPlayerUpdate(pt->GetType(), pt->GetTickCount());
                if (_currentView->HasModal()) {
                    _currentView->ForwardPlayerUpdateToModal(pt->GetType(), pt->GetTickCount());
                }
                Invalidate();
            }
        }

        break;
    }

        /*	  case VET_LIST_SELECT:
              {
                char *name=(char*)ve->GetData() ;
                LoadProject(name) ;
                break ;
              } */

    case VET_SAVEAS_PROJECT: {
        char *name = (char *)ve->GetData();
        _loadAfterSaveAsProject = true;
        _newProjectToLoad = name;
        break;
    }

    case VET_QUIT_PROJECT: {
        // defer event to after we got out of the view
        _closeProject = true;
        break;
    }
    case VET_QUIT_APP:
        _shouldQuit = true;
        break;
    }
}

void AppWindow::onQuitApp() {
    Player *player = Player::GetInstance();
    player->Stop();
    player->RemoveObserver(*this);

    player->Reset();
    System::GetInstance()->PostQuitMessage();
}
void AppWindow::Print(char *line) {

    //	GUIWindow::Clear(View::backgroundColor_,true) ;
    Clear();
    snprintf(_statusLine, sizeof(_statusLine), "%s", line);
    // unwrapped for gcc
    int position = LOGICAL_COLS;
    position -= strlen(_statusLine);
    position /= 2;
    GUIPoint pos(position, 12);
    //
    GUITextProperties props;
    SetColor(CD_NORMAL);
    DrawString(_statusLine, pos, props);
    char buildString[80];
    sprintf(buildString, "Piggy build %s.%s.%s", PROJECT_NUMBER,
            PROJECT_RELEASE, BUILD_COUNT);
    pos._y = 28;
    pos._x = (LOGICAL_COLS - strlen(buildString)) / 2;
    DrawString(buildString, pos, props);
    Flush();
};

void AppWindow::SetColor(ColorDefinition cd) { colorIndex_ = cd; };

void AppWindow::InvalidateScreenCache() {
    memset(_preScreen, 0xFF, LOGICAL_SIZE);
    memset(_preScreenProp, 0xFF, LOGICAL_SIZE);
};

Path AppWindow::GetLastProjectPath() {
    Path lastProjectFile(LAST_PROJECT_NAME);
    FileSystem *fs = FileSystem::GetInstance();
    I_File *file = fs->Open(lastProjectFile.GetPath().c_str(), "r");

    if (!file) {
        return Path();
    }

    // Get file size
    file->Seek(0, SEEK_END);
    int length = file->Tell();
    
    if (length <= 0) {
        file->Close();
        delete file;
        return Path();
    }

    // Allocate buffer and seek back to start
    char *buffer = (char *)SYS_MALLOC(length + 1);
    memset(buffer, 0, length + 1);

    file->Seek(0, SEEK_SET); // Seek back to start
    int bytes = file->Read(buffer, 1, length); // Read full length
    file->Close();
    delete file;

    if (bytes <= 0) {
        Trace::Error("GetLastProject: Failed to read last project file");
        SYS_FREE(buffer);
        return Path();
    }

    buffer[bytes] = 0; // Null terminate

    // Remove newline if present
    int len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
        buffer[len-1] = 0;
    }

    Path result;
    if (strlen(buffer) > 0) {
        if (strstr(buffer, "lgpt_") != NULL) { // Ensure it's an lgpt project
            result = Path(buffer);
        } else {
            Trace::Error("GetLastProject: Invalid project path format: %s",
                         buffer);
        }
    }
    if (!result.IsDirectory()) {
        Trace::Error("GetLastProject: path does not exist: %s", result.GetPath().c_str());
    }

    SYS_FREE(buffer);
    return result;
}

void AppWindow::SaveLastProject(const Path &p) {
    Path lastProjectFile(LAST_PROJECT_NAME);
    FileSystem *fs = FileSystem::GetInstance();
    I_File *file = fs->Open(lastProjectFile.GetPath().c_str(), "w");

    if (!file) {
        Trace::Error("SaveLastProject: Failed to open %s for writing",
                     LAST_PROJECT_NAME);
        return;
    }

    std::string pathStr = p.GetPath();
    file->Write(pathStr.c_str(), 1, pathStr.length());
    file->Write("\n", 1, 1);
    file->Close();
    delete file;

    
}
