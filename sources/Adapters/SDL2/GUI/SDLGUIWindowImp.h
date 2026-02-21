#ifndef SDL_GUI_WINDOW_H_
#define SDL_GUI_WINDOW_H_

#include "UIFramework/Interfaces/I_GUIWindowImp.h"
#include <SDL2/SDL.h>

bool ProcessDEBEvent(SDL_Event &event) ;
void ProcessButtonChange(unsigned short,unsigned short) ;

#define MAX_OVERLAYS 250

struct SDLCreateWindowParams: public GUICreateWindowParams {
	SDLCreateWindowParams():cacheFonts_(true),framebuffer_(false) {} ;
	bool cacheFonts_ ;
	bool framebuffer_ ;
} ;

class SDLGUIWindowImp: public I_GUIWindowImp {

public:

	SDLGUIWindowImp(GUICreateWindowParams &p) ;
	virtual ~SDLGUIWindowImp() ;

public: // I_GUIWindowImp implementation

	virtual void SetColor(GUIColor &) ;
	virtual void DrawRect(GUIRect &) ;
	virtual void DrawChar(const char c,GUIPoint &pos,GUITextProperties &);
	virtual void DrawString(const char *string,GUIPoint &pos,GUITextProperties &,bool overlay=false);
	virtual GUIRect GetRect() ;
	virtual void Invalidate() ;
	virtual void Flush();
	virtual void Lock() ;
	virtual void Unlock() ;
	virtual void Clear(GUIColor &, bool overlay=false) ;
	virtual void ClearRect(GUIRect &) ;
	virtual void PushEvent(GUIEvent &event) ;

	// Present the renderer (call after DrawGraphics overlay drawing)
	void Present();

	// Access renderer for hardware-accelerated overlay drawing
	SDL_Renderer *GetRenderer() { return renderer_; }
	int GetAppAnchorX() const { return appAnchorX_; }
	int GetAppAnchorY() const { return appAnchorY_; }
	int GetMult() const { return mult_; }

public: // Added functionality
	void ProcessExpose() ;
	void ProcessQuit() ;
	void ProcessUserEvent(SDL_Event &event) ;
protected:
	void prepareFonts() ;
	void prepareFullFonts() ;
	void prepareBPP1Fonts() ;
  void transform(const GUIRect &srcRect,SDL_Rect *dstRect);
  void transform(const GUIPoint &srcPoint, int *x, int *y);

private:
    SDL_Window *window_;
    SDL_Renderer *renderer_;       // Hardware-accelerated renderer
    SDL_Texture *screenTexture_;   // Streaming texture for software-drawn content
    SDL_Surface *surface_;         // Software drawing surface (backbuffer)
    GUIRect screenRect_ ;
	unsigned int currentColor_ ;
	unsigned int backgroundColor_ ;
	unsigned int foregroundColor_ ;
	int bitDepth_ ;
	bool cacheFonts_ ;
	bool framebuffer_ ;
  bool windowed_;
	SDL_Rect updateRects_[MAX_OVERLAYS] ;
	int updateCount_ ;
	int appAnchorX_ ;
	int appAnchorY_ ;
	int mult_ ;
} ;
#endif
