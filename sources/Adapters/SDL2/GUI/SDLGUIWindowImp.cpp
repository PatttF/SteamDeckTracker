#include "SDLGUIWindowImp.h"
#include <assert.h>
#include <string.h>
#include "Application/Model/Config.h"
#include "Application/Utils/char.h"
#include "System/Console/n_assert.h"
#include "System/Console/Trace.h"
#include "System/System/System.h"
#include "UIFramework/BasicDatas/GUIEvent.h"
#include "UIFramework/SimpleBaseClasses/GUIWindow.h"
#include "UIFramework/BasicDatas/FontConfig.h"

SDLGUIWindowImp *instance_ ;

unsigned short appWidth=550 ;
unsigned short appHeight=240 ;

SDLGUIWindowImp::SDLGUIWindowImp(GUICreateWindowParams &p) 
{

  SDLCreateWindowParams &sdlP=(SDLCreateWindowParams &)p;
  cacheFonts_=sdlP.cacheFonts_ ;
  framebuffer_=sdlP.framebuffer_ ;
  
  // By default if we are not running a framebuffer device
  // we assumed it's windowed

  windowed_ = !framebuffer_;


  SDL_DisplayMode displayMode;

  // SDL Prioritises screens so just take the first for now.
  
  int displayModeRet = SDL_GetDisplayMode(0, 0, &displayMode);
    
  if (displayModeRet < 0) {
    Trace::Error("DISPLAY","No display mode found.  Error Code: %d.", displayModeRet);
  }
    
  NAssert(displayModeRet >= 0);
 
 #if defined(PLATFORM_PSP)
  int screenWidth = 480; 
  int screenHeight = 272;
  windowed_ = false;
 #elif defined(RS97)
  int screenWidth = 320; 
  int screenHeight = 240;
  windowed_ = false;
 #else
  int screenWidth = displayMode.w;
  int screenHeight = displayMode.h;
 #endif
 
 #if defined(RS97)
  /* Pick the best bitdepth for the RS97 as it will select 32 as its default, even though that's slow */
  bitDepth_ = 16;
 #else
  bitDepth_ = SDL_BITSPERPIXEL(displayMode.format);
 #endif
  
  const char * driverName = SDL_GetVideoDriver(0);
  
  
  
  bool fullscreen=false ;
  
  const char *fullscreenValue=Config::GetInstance()->GetValue("FULLSCREEN") ;
  if ((fullscreenValue)&&(!strcmp(fullscreenValue,"YES")))
  {
  	fullscreen=true ;
  }
  	
  if (!strcmp(driverName, "fbcon"))
  {
    framebuffer_ = true;
    windowed_ = false;
  }
 
  #ifdef PLATFORM_PSP
  	mult_ = 1;
  #else
	int multFromSize=MIN(screenHeight/appHeight,screenWidth/appWidth);
	const char *mult=Config::GetInstance()->GetValue("SCREENMULT") ;
	if (mult)
	{
		mult_=atoi(mult);
	}
	else
	{
		if (framebuffer_)
		{
		mult_ = multFromSize;
		}
		else
		{
		mult_ = 1;
		}
	}
  #endif
  // Create a window that is the requested size
  
  screenRect_._topLeft._x=0;
  screenRect_._topLeft._y=0;
  screenRect_._bottomRight._x=windowed_?appWidth*mult_:screenWidth;
  screenRect_._bottomRight._y=windowed_?appHeight*mult_:screenHeight;

  
    window_ = SDL_CreateWindow("SDTracker",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,
                               screenRect_.Width(),screenRect_.Height(),fullscreen?SDL_WINDOW_FULLSCREEN:SDL_WINDOW_SHOWN);
    NAssert(window_) ;

	// Compute the x & y offset to locate our app window

	appAnchorX_=(screenRect_.Width()-appWidth*mult_)/2 ;
	appAnchorY_=(screenRect_.Height()-appHeight*mult_)/2 ;

    SDL_Surface *icon = SDL_LoadBMP("lgpt_icon.bmp");
    if (icon) {
        SDL_SetWindowIcon(window_, icon);
        SDL_FreeSurface(icon);
    }

    // Create hardware-accelerated renderer (no VSYNC — frame rate is managed
    // manually to avoid blocking the main thread and starving the audio thread)
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) {
        // Fallback to software renderer
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    NAssert(renderer_);

    // Create a software surface as our drawing backbuffer
    surface_ = SDL_CreateRGBSurface(0, screenRect_.Width(), screenRect_.Height(),
                                     32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    NAssert(surface_);

    // Create a streaming texture to upload the software surface to the renderer
    screenTexture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        screenRect_.Width(), screenRect_.Height());
    NAssert(screenTexture_);

    Uint32 rmask, gmask, bmask, amask;

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    rmask = 0xff000000;
    gmask = 0x00ff0000;
    bmask = 0x0000ff00;
    amask = 0x000000ff;
#else
    rmask = 0x000000ff;
    gmask = 0x0000ff00;
    bmask = 0x00ff0000;
    amask = 0xff000000;
#endif

	instance_=this ;
	currentColor_=0;
	backgroundColor_=0 ;
	SDL_ShowCursor(SDL_DISABLE);
	FontConfig();

	
	if (cacheFonts_)
  {
    
		prepareFonts() ;
	}
	updateCount_=0 ;
} ;

SDLGUIWindowImp::~SDLGUIWindowImp() {
    if (screenTexture_) { SDL_DestroyTexture(screenTexture_); screenTexture_ = nullptr; }
    if (surface_) { SDL_FreeSurface(surface_); surface_ = nullptr; }
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
}

static SDL_Surface *fonts[FONT_COUNT] ;

void SDLGUIWindowImp::prepareFullFonts()
{
  
  Uint32 rmask, gmask, bmask, amask;

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
  rmask = 0xff000000;
  gmask = 0x00ff0000;
  bmask = 0x0000ff00;
  amask = 0x000000ff;
#else
  rmask = 0x000000ff;
  gmask = 0x0000ff00;
  bmask = 0x00ff0000;
  amask = 0xff000000;
#endif

	for (int i=0;i<FONT_COUNT;i++)
  {
    
	  fonts[i] = SDL_CreateRGBSurface(
                 SDL_SWSURFACE,
                 8*mult_, 8*mult_, 
                 bitDepth_,
                 0, 0, 0, 0);
		if (fonts[i]==NULL) 
    {
			Trace::Error("[DISPLAY] Failed to create font surface %d",i) ;
		}
    else
    {
			SDL_LockSurface(fonts[i]) ;
			int pixelSize=fonts[i]->format->BytesPerPixel ;
			unsigned char *bgPtr=(unsigned char *)&backgroundColor_ ;
			unsigned char *fgPtr=(unsigned char *)&foregroundColor_ ;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
			bgPtr+=(4-pixelSize) ;
			fgPtr+=(4-pixelSize) ;
#endif
			const unsigned char *src=font+i*8 ;
			unsigned char *dest=(unsigned char *)fonts[i]->pixels;
      for (int y = 0; y < 8; y++)
      {
				for (int n=0;n<mult_;n++)
        {
					for (int x = 0; x < 8; x++)
          {
						unsigned char *pxlPtr=src[x]?bgPtr:fgPtr ;
						for (int m=0;m<mult_;m++)
            {
							memcpy(dest,pxlPtr,pixelSize) ;
							dest+=pixelSize ;
						}
					}
					dest+=fonts[i]->pitch-8*pixelSize*mult_ ;
        }
				if (y<7) src+=FONT_WIDTH ;
      }
			SDL_UnlockSurface(fonts[i]) ;
		};
	};
}

void SDLGUIWindowImp::prepareFonts() 
{

  
  Config *config=Config::GetInstance() ;
  
  unsigned char r,g,b ;
  const char *value=config->GetValue("BACKGROUND") ;
  if (value)
  {
    char2hex(value,&r) ;
    char2hex(value+2,&g) ;
    char2hex(value+4,&b) ;
  } 
  else
  {
    r=0xF1 ;
    g=0xF1 ;
    b=0x96 ;      
  }
  backgroundColor_=SDL_MapRGB(surface_->format, r,g,b) ;
           
  value=config->GetValue("FOREGROUND") ;
  if (value)
  {
    char2hex(value,&r) ;
    char2hex(value+2,&g) ;
    char2hex(value+4,&b) ;
  } 
  else
  {
    r=0x77 ;
    g=0x6B ;
    b=0x56 ;      
  }
  foregroundColor_=SDL_MapRGB(surface_->format, r,g,b) ;
        
  prepareFullFonts() ;
}

void SDLGUIWindowImp::DrawChar(const char c, GUIPoint &pos, GUITextProperties &p) 
{
  int xx,yy;
  transform(pos, &xx, &yy);

  if ((xx<0) || (yy<0)) return;
  if ((xx>=screenRect_._bottomRight._x) || (yy>=screenRect_._bottomRight._y))
       return ;
	if ((!framebuffer_)&&(updateCount_<MAX_OVERLAYS)) {
		SDL_Rect *area=updateRects_+updateCount_++ ;
		area->x=xx ;
		area->y=yy ;
		area->h=8*mult_ ;
		area->w=8*mult_ ;
	}

	if (((cacheFonts_)&&(currentColor_==foregroundColor_)&&(!p.invert_))) {

		SDL_Rect srcRect ;
		srcRect.x=0 ;
		srcRect.y=0 ;
		srcRect.w=8*mult_ ;
		srcRect.h=8*mult_ ;

		SDL_Rect dstRect ;
		dstRect.x=xx ;
		dstRect.y=yy ;
		dstRect.w=8*mult_ ;
		dstRect.h=8*mult_ ;

		unsigned int fontID=c ;
		if (fontID<FONT_COUNT) {
            SDL_BlitSurface(fonts[fontID], &srcRect,surface_, &dstRect);
		}

	} else {
		// Bounds-check the character index to prevent out-of-bounds font access
		unsigned int fontID = (unsigned char)c;
		if (fontID >= FONT_COUNT) return;

		// prepare bg & fg pixel ptr
        int pixelSize=surface_->format->BytesPerPixel ;
		unsigned char *bgPtr=(unsigned char *)&backgroundColor_ ;
		unsigned char *fgPtr=(unsigned char *)&currentColor_ ;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
		bgPtr+=(4-pixelSize) ;
		fgPtr+=(4-pixelSize) ;
#endif
		const unsigned char *src=font+fontID*8 ;
        unsigned char *dest=((unsigned char *)surface_->pixels) + (yy*surface_->pitch) + xx*pixelSize;

		for (int y = 0; y < 8; y++) {
			for (int n=0;n<mult_;n++) {
				for (int x = 0; x < 8; x++) {
					unsigned char *pxlPtr=((src[x])^(unsigned char)p.invert_)?bgPtr:fgPtr ; ;
					for (int m=0;m<mult_;m++) {
						memcpy(dest,pxlPtr,pixelSize) ;
						dest+=pixelSize ;
					}
				}
                dest+=surface_->pitch-8*pixelSize*mult_ ;
    		}
			if (y<7) src+=FONT_WIDTH ;
  		}

	}
}

void SDLGUIWindowImp::transform(const GUIRect &srcRect,SDL_Rect *dstRect)
{
  dstRect->x = srcRect.Left() * mult_ + appAnchorX_;
  dstRect->y = srcRect.Top() * mult_  + appAnchorY_;
  dstRect->w = srcRect.Width() * mult_ ;
  dstRect->h = srcRect.Height() * mult_ ; 
}

void SDLGUIWindowImp::transform(const GUIPoint &srcPoint, int *x, int *y)
{
 	*x=appAnchorX_ + srcPoint._x*mult_ ;
	*y=appAnchorY_ + srcPoint._y*mult_ ; 
}

void SDLGUIWindowImp::DrawString(const char *string,GUIPoint &pos,GUITextProperties &p,bool overlay) 
{

	int len=int(strlen(string)) ;
  int xx,yy;
  transform(pos, &xx , &yy);

	if ((!framebuffer_)&&(updateCount_<MAX_OVERLAYS))
  {
		SDL_Rect *area=updateRects_+updateCount_++ ;
		area->x=xx ;
		area->y=yy ;
		area->h=8*mult_ ;
		area->w=len*8*mult_ ;
	}

	for (int l=0;l<len;l++)
  {
		if (((cacheFonts_)&&(currentColor_==foregroundColor_)&&(!p.invert_)))
    {
			SDL_Rect srcRect ;
			srcRect.x=0 ;
			srcRect.y=0 ;
			srcRect.w=8*mult_ ;
			srcRect.h=8*mult_ ;

			SDL_Rect dstRect ;
			dstRect.x=xx ;
			dstRect.y=yy ;
			dstRect.w=8*mult_ ;
			dstRect.h=8*mult_ ;

			unsigned int fontID=string[l] ;
			if (fontID<FONT_COUNT)
      {
                SDL_BlitSurface(fonts[fontID], &srcRect,surface_, &dstRect);
			}
		} 
    else
    {
			// Bounds-check the character index to prevent out-of-bounds font access
			unsigned int fontID = (unsigned char)string[l];
			if (fontID >= FONT_COUNT) { xx+=8*mult_; continue; }

			// prepare bg & fg pixel ptr
            int pixelSize=surface_->format->BytesPerPixel ;
			unsigned char *bgPtr=(unsigned char *)&backgroundColor_ ;
			unsigned char *fgPtr=(unsigned char *)&currentColor_ ;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
			bgPtr+=(4-pixelSize) ;
			fgPtr+=(4-pixelSize) ;
#endif
			const unsigned char *src=font+(fontID*8) ;
            unsigned char *dest=((unsigned char *)surface_->pixels) + (yy*surface_->pitch) + xx*pixelSize;

			for (int y = 0; y < 8; y++) {
				for (int n=0;n<mult_;n++) {
					for (int x = 0; x < 8; x++) {
						unsigned char *pxlPtr=((src[x])^(unsigned char)p.invert_)?bgPtr:fgPtr ; ;
						for (int m=0;m<mult_;m++) {
							memcpy(dest,pxlPtr,pixelSize) ;
							dest+=pixelSize ;
						}
					}
                    dest+=surface_->pitch-8*pixelSize*mult_ ;
        		}
				if (y<7) src+=FONT_WIDTH ;
	  		}

		}
    xx+=8*mult_ ;
	}
}

void SDLGUIWindowImp::DrawRect(GUIRect &r) 
{
  SDL_Rect rect;
  transform(r, &rect);
  SDL_FillRect(surface_, &rect,currentColor_) ;

  // Track update rect so Flush presents these pixels to screen
  if (!framebuffer_) {
    if (updateCount_ < MAX_OVERLAYS) {
      updateRects_[updateCount_] = rect;
    }
    updateCount_++;
  }
} ;

void SDLGUIWindowImp::Clear(GUIColor &c,bool overlay) 
{
  SDL_Rect rect;
  rect.x = 0;
  rect.y = 0;
  rect.w = screenRect_.Width();
  rect.h = screenRect_.Height();
 
    backgroundColor_=SDL_MapRGB(surface_->format,c._r&0xFF,c._g&0xFF,c._b&0xFF);
  SDL_FillRect(surface_, &rect,backgroundColor_) ;

	if (!framebuffer_)
  {		
		SDL_Rect *area=updateRects_;
		area->x=rect.x ;
		area->y=rect.y ;
		area->w=rect.w ;
		area->h=rect.h ;
		updateCount_=1 ;
	}
}

void SDLGUIWindowImp::ClearRect(GUIRect &r) 
{
  SDL_Rect rect;
  transform(r, &rect);
  SDL_FillRect(surface_, &rect,backgroundColor_) ;
}

// To the app we might have a smaller window
// than the effective one (PSP)

GUIRect SDLGUIWindowImp::GetRect() 
{
	return GUIRect(0,0,appWidth,appHeight) ;
}

// Pushback a SDL event to specify screen has to be redrawn.

void SDLGUIWindowImp::Invalidate() 
{
    // Zero-initialize the event structure to avoid passing uninitialized data into SDL
    // (Valgrind reported conditional jumps depending on uninitialized values here)
    SDL_Event event ;
    SDL_memset(&event, 0, sizeof(event));
    event.type = SDL_WINDOWEVENT ;
    event.window.event = SDL_WINDOWEVENT_EXPOSED;
    SDL_PushEvent(&event) ;
}

void SDLGUIWindowImp::SetColor(GUIColor &c) 
{
    currentColor_=SDL_MapRGB(surface_->format,c._r&0xFF,c._g&0xFF,c._b&0xFF);
}

void SDLGUIWindowImp::Lock() 
{
  if (framebuffer_)
  {
    return;
  }

    if (SDL_MUSTLOCK(surface_))
  {
        SDL_LockSurface(surface_) ;
	}
}

void SDLGUIWindowImp::Unlock() 
{
  if (framebuffer_)
  {
    return;
  }

    if (SDL_MUSTLOCK(surface_))
  {
        SDL_UnlockSurface(surface_) ;
	}
}

void SDLGUIWindowImp::Flush()
{
    // Upload the software surface to the streaming texture
    SDL_UpdateTexture(screenTexture_, NULL, surface_->pixels, surface_->pitch);

    // Clear the renderer and copy the texture (the full software-drawn frame)
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, screenTexture_, NULL, NULL);

    // Do NOT call SDL_RenderPresent here — that happens in Present()
    // after DrawGraphics() has added hardware-accelerated overlays
    updateCount_=0;
}

void SDLGUIWindowImp::Present()
{
    SDL_RenderPresent(renderer_);
}

void SDLGUIWindowImp::ProcessExpose() 
{
    // With renderer-based presentation, we just need to trigger a redraw
    _window->Update() ;
}

void SDLGUIWindowImp::ProcessQuit()
{
	GUIPoint p;
	GUIEvent e(p,ET_SYSQUIT) ;
	_window->DispatchEvent(e) ;	
} ;

void SDLGUIWindowImp::PushEvent(GUIEvent &event)
{
	// Create a heap copy so the queued SDL event owns a pointer we can delete later
	GUIEvent *heapEvent = new GUIEvent(event);
	SDL_Event sdlevent ;
	sdlevent.type = SDL_USEREVENT ;
	sdlevent.user.data1 = heapEvent ;
	SDL_PushEvent(&sdlevent) ;
} ;

void SDLGUIWindowImp::ProcessUserEvent(SDL_Event &event)
{
	GUIEvent *guiEvent=(GUIEvent *)event.user.data1 ;
	_window->DispatchEvent(*guiEvent) ;
	delete(guiEvent) ;
}
