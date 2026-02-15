#include "Application/Application.h" 
#include "Application/AppWindow.h" 
#include "UIFramework/Interfaces/I_GUIWindowFactory.h"
#include "Application/Persistency/PersistencyService.h" 
#include "Services/Audio/Audio.h"
#include "Application/Commands/CommandDispatcher.h"
#include "Application/Controllers/ControlRoom.h"
#include "Application/Utils/ProtonBridge.h"
#include <sys/stat.h>
#include <stdio.h>
#include <fstream>
#include "Application/Model/Config.h"
#include "Services/Midi/MidiService.h"

#include <math.h>
#include <stdlib.h>

Application *Application::instance_=NULL ;

Application::Application() {
}

void Application::initMidiInput()
{
  const char *preferedDevice=Config::GetInstance()->GetValue("MIDICTRLDEVICE");

  IteratorPtr<MidiInDevice>it(MidiService::GetInstance()->GetInIterator()) ;
  for(it->Begin();!it->IsDone();it->Next())
  {
    MidiInDevice &in=it->CurrentItem() ;
    if ((preferedDevice) && (!strncmp(in.GetName(), preferedDevice, strlen(preferedDevice))))
    {
      if (in.Init())
      {
        if (in.Start())
        {
          
        }
        else
        {
          in.Close() ;
        }
      }
    }
  }
}

bool Application::Init(GUICreateWindowParams &params) {
	const char* root=Config::GetInstance()->GetValue("ROOTFOLDER") ;
	if (root) {
		Path::SetAlias("root",root) ;
	} ;
	window_=AppWindow::Create(params) ;
	PersistencyService::GetInstance() ;

  Audio *audio=Audio::GetInstance() ;
  audio->Init() ;
	CommandDispatcher::GetInstance()->Init() ;
  initMidiInput();
  // Try loading mapping from user root (~/Documents/SDTracker) first, then fall back to the binary directory
  if (!ControlRoom::GetInstance()->LoadMapping("root:mapping.xml")) {
    if (!ControlRoom::GetInstance()->LoadMapping("bin:mapping.xml")) {
      // No mapping file found: write a sensible keyboard fallback into root:mapping.xml
      Path p("root:mapping.xml") ;
      std::string mapPath = p.GetPath();

      // Ensure parent directory exists (best-effort)
      size_t pos = mapPath.find_last_of('/');
      if (pos!=std::string::npos) {
        std::string dir = mapPath.substr(0,pos);
        struct stat st ;
        if (stat(dir.c_str(),&st)!=0) {
          mkdir(dir.c_str(),0755);
        }
      }

            // Write mapping using C++ streams to avoid any fopen macro conflicts
            const char *defaultMap = R"MAP(<!--
          MAPPING FOR KEYBOARDS
          belongs in location of binary
          -=-=These keys might not match your controller!=-=-
          enable DUMPEVENT to help with mapping
      -->
      <MAPPINGS>

      <!--
          EXAMPLE
          maps the button "q" to left shoulder
          maps the button "w" to right shoulder
      -->
        <MAP src="key:0:q" dst="/event/lshoulder" />
        <MAP src="key:0:w" dst="/event/rshoulder" />

        <MAP src="key:0:up" dst="/event/up" />
        <MAP src="key:0:down" dst="/event/down" />
        <MAP src="key:0:left" dst="/event/left" />
        <MAP src="key:0:right" dst="/event/right" />

        <MAP src="key:0:a" dst="/event/b" />
        <MAP src="key:0:s" dst="/event/a" />
        <MAP src="key:0:space" dst="/event/start" />
        <MAP src="key:0:e" dst="/event/select" />
        <MAP src="key:0:," dst="/event/pgback" />
        <MAP src="key:0:." dst="/event/pgfwd" />
      <!--
          EXAMPLE
          maps the button "8" to left shoulder
          maps the button "6" to right shoulder
      -->
        <MAP src="but:4:4" dst="/event/rshoulder" />
        <MAP src="but:4:5" dst="/event/lshoulder" />

        <MAP src="hat:0:0" dst="/event/up" />
        <MAP src="hat:0:2" dst="/event/down" />
        <MAP src="hat:0:3" dst="/event/left" />
        <MAP src="hat:0:1" dst="/event/right" />

        <MAP src="but:1:1" dst="/event/a" />
        <MAP src="but:0:0" dst="/event/b" />
        <MAP src="but:3:3" dst="/event/start" />

      <!--
      Macro 1
        Stop Phrase (Selected phrase cue stop)
        <MAP src="but:0:3" dst="/event/rshoulder" />
        <MAP src="but:0:3" dst="/event/start" />
      -->
      <!--
      Macro 2
        Queue Phrases (Current row Phrases cue play)
        <MAP src="but:0:2" dst="/event/lshoulder" />
        <MAP src="but:0:2" dst="/event/start" />
      -->
      <!--
      Macro 3
        Solo phrase (hold to solo, release to unsolo)
        Warning: will do weirdness if combined with other buttons! Use with caution ;)
        <MAP src="but:0:5" dst="/event/a" />
        <MAP src="but:0:5" dst="/event/rshoulder" />
      -->
      </MAPPINGS>
      )MAP";

            std::ofstream out(mapPath.c_str());
            if (out) {
        out << defaultMap;
        out.close();
        
            } else {
        
            }

      // Load the mapping we just wrote (or try again)
      ControlRoom::GetInstance()->LoadMapping("root:mapping.xml");
      // Also install runtime attachments as safety net
      ControlRoom::GetInstance()->InstallDefaultMapping();
    }
  }

  // If no custom font exists in root:, write a default custom_font.xml so the app can load it
  {
    Path fp("root:custom_font.xml");
    std::string fontPath = fp.GetPath();
    struct stat stt;
    if (stat(fontPath.c_str(), &stt) != 0) {
      size_t ppos = fontPath.find_last_of('/');
      if (ppos!=std::string::npos) {
        std::string dir = fontPath.substr(0,ppos);
        struct stat st2;
        if (stat(dir.c_str(),&st2)!=0) {
          mkdir(dir.c_str(),0755);
        }
      }
      const char *defaultFont = R"FONT(<FONT>
<DATA value ='1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,0,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,0,1,0,1,1,1, 1,1,0,1,0,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,0,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,0,0,1,1,1, 1,1,0,0,1,1,1,1, 1,0,1,0,1,0,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,0,0,0,0,0,0,1, 1,1,1,1,1,0,0,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,1, 1,0,1,1,1,1,0,1, 1,0,0,0,0,0,0,1, 1,0,1,1,1,1,1,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,0,1,1, 1,1,1,1,1,1,1,1, 1,1,0,1,1,1,1,1, 1,0,0,0,0,0,1,1, 1,0,0,0,0,0,0,1, 1,1,0,0,0,0,1,1, 1,0,0,0,0,0,1,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,1, 1,0,1,1,1,1,0,1, 1,1,1,1,0,1,1,1, 1,1,1,1,1,1,0,1, 1,0,1,1,1,1,0,1, 1,0,1,1,1,1,1,1, 1,0,1,1,1,1,0,1, 1,0,1,1,1,1,0,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,1, 1,0,0,0,0,1,1,1, 1,1,0,0,0,0,0,1, 1,0,1,1,1,1,0,1, 1,0,1,1,1,1,0,1, 1,0,1,1,1,1,0,1, 1,0,1,1,1,1,0,1, 1,0,1,1,1,1,0,1, 1,0,0,0,0,0,0,1, 1,1,0,0,0,0,1,1, 1,1,1,1,1,1,1,1, 1,1,0,0,0,0,1,1, 1,1,1,0,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,0,0,0,1, 1,1,1,1,0,1,1,1, 1,0,0,0,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 
' /></FONT>
)FONT";
      std::ofstream fout(fontPath.c_str());
      if (fout) {
        fout << defaultFont;
        fout.close();
        
      } else {
        
      }
    }
  }

  // If installer script missing in root:scripts, try to copy from bundled bin: locations
  {
    Path dst("root:scripts/install_lv2_steamos.sh");
    std::string dstPath = dst.GetPath();
    struct stat st;
    if (stat(dstPath.c_str(), &st) != 0) {
      // not present; try candidate sources under bin:
      const char *candidates[] = {"bin:scripts/install_lv2_steamos.sh","bin:../scripts/install_lv2_steamos.sh","bin:../../scripts/install_lv2_steamos.sh"};
      bool copied = false;
      for (size_t i=0;i<sizeof(candidates)/sizeof(candidates[0]);++i) {
        Path src(candidates[i]);
        if (FileSystem::GetInstance()->GetFileType(src.GetPath().c_str())==FT_FILE) {
          // ensure destination directory exists
          size_t ppos = dstPath.find_last_of('/');
          if (ppos!=std::string::npos) {
            std::string dir = dstPath.substr(0,ppos);
            struct stat st2;
            if (stat(dir.c_str(),&st2)!=0) {
              mkdir(dir.c_str(),0755);
            }
          }
          // copy file contents
          std::string srcCanon = src.GetCanonicalPath();
          std::ifstream in(srcCanon.c_str(), std::ios::binary);
          std::ofstream out(dstPath.c_str(), std::ios::binary);
          if (in && out) {
            out << in.rdbuf();
            out.close();
            in.close();
            chmod(dstPath.c_str(),0755);
            
            copied = true;
            break;
          } else {
            
          }
        }
      }
      if (!copied) {
        
      }
    }
  }

  // Bridge Windows VST3 plugins using bundled yabridge + Proton
  bridgeWindowsVST3s();

	return true ;
} ;

GUIWindow *Application::GetWindow() {
	return window_ ;
} ;

Application::~Application() {
	delete window_ ;
}
