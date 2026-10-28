#pragma once
#include <SDL.h>
#include <cstdint>
namespace uu {struct Picture;}
namespace uu::display {
enum class PresentStatus {Submitted,Skipped,SdlPresented};
struct PresentResult {
    PresentStatus status=PresentStatus::Skipped;
    uint64_t videoSerial=0;
    bool directVideo=false;
    uint64_t slotWaitUs=0;
    uint64_t acquireUs=0;
    uint64_t submitUs=0;
    uint64_t presentUs=0;
};
bool nativeRequested();
SDL_Renderer *createRenderer(SDL_Window *&,int,int);
bool active();
void shutdown();
void setColor(SDL_Renderer *,Uint8,Uint8,Uint8,Uint8);
int fillRect(SDL_Renderer *,const SDL_Rect *);
int clear(SDL_Renderer *);
PresentResult present(SDL_Renderer *);
SDL_Texture *textureFromSurface(SDL_Renderer *,SDL_Surface *);
void destroyTexture(SDL_Texture *);
int copy(SDL_Renderer *,SDL_Texture *,const SDL_Rect *,const SDL_Rect *);
int vsync(SDL_Renderer *,int);
bool acceptVideo(const Picture &);
void drawVideo(const SDL_Rect &);
void resetVideo();
uint64_t directFrames(); // native video frames submitted to GPU; not accepted count, not scanout
uint64_t mapHits();
uint64_t mapImports();
uint64_t mapCached();
}
