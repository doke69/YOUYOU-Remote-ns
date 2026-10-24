#include "native_display.hpp"
#include "media.hpp"
#include "config_store.hpp"
#include "diagnostics.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef __SWITCH__
#include "native_nv12.hpp"

namespace uu::display {
namespace {
constexpr uint32_t slots=3,commandBytes=1<<20,uniformBytes=1<<18,descriptors=512;
uint32_t aligned(uint32_t value,uint32_t alignment) {return (value+alignment-1)&~(alignment-1);}
dk::UniqueMemBlock memory(dk::Device device,uint32_t bytes,uint32_t flags) {
    auto block=dk::MemBlockMaker{device,aligned(bytes,DK_MEMBLOCK_ALIGNMENT)}.setFlags(flags).create();
    if(!block) throw std::runtime_error("GPU 内存分配失败");
    return block;
}
constexpr uint32_t sharedFlags=DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached;
struct Texture {
    int width=0,height=0;uint32_t pitch=0;
    dk::UniqueMemBlock storage;
    dk::ImageLayout layout;
    dk::Image image;
    dk::ImageDescriptor descriptor;
};
struct Slot {
    dk::UniqueMemBlock commands,uniforms,images;
    dk::UniqueCmdBuf command;
    dk::Fence fence{};
    bool pending=false;
    uint32_t used=0,imageCount=0;
    std::vector<std::shared_ptr<Texture>> textures;
    std::shared_ptr<NativeNv12Surface> video;
};
struct Renderer {
    dk::UniqueDevice device;
    dk::UniqueQueue queue;
    dk::UniqueMemBlock frameMemory,code,samplerMemory;
    std::array<dk::Image,slots> frameImages;
    dk::UniqueSwapchain swapchain;
    std::array<Slot,slots> frames;
    std::array<dk::Shader,4> shaders;
    std::map<SDL_Texture *,std::shared_ptr<Texture>> textures;
    std::shared_ptr<Texture> white,cpuVideo;
    std::shared_ptr<NativeNv12Surface> nativeVideo;
    NativeNv12Cache nativeCache;
    SDL_Surface *fontSurface=nullptr;
    SDL_Color color{255,255,255,255};
    int width=1280,height=720,imageSlot=-1;unsigned current=0;
    uint64_t hardwareFrames=0;
    bool recording=false;
    Renderer(int w,int h):width(w),height(h) {
        auto *nativeWindow=nwindowGetDefault();
        // Catch ownership conflicts before deko3d's nonrecoverable error path.
        if(!nwindowIsValid(nativeWindow) || nativeWindow->slots_configured) {
            diagnostic("display-window-not-exclusive");
            throw std::runtime_error("Native window unavailable");
        }
        const auto windowResult=nwindowSetDimensions(nativeWindow,w,h);
        if(R_FAILED(windowResult)) {
            diagnostic(("display-window-result-"+std::to_string(windowResult)).c_str());
            throw std::runtime_error("Native window dimensions failed");
        }
        diagnostic("display-native-device-begin");
        device=dk::DeviceMaker{}.setFlags(DkDeviceFlags_OriginUpperLeft|DkDeviceFlags_YAxisPointsDown).create();
        if(!device) throw std::runtime_error("Deko 初始化失败");
        queue=dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();
        if(!queue) throw std::runtime_error("GPU 队列创建失败");
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}.setDimensions(w,h).setFormat(DkImageFormat_RGBA8_Unorm)
            .setFlags(DkImageFlags_UsageRender|DkImageFlags_UsagePresent).initialize(layout);
        const auto stride=aligned(uint32_t(layout.getSize()),layout.getAlignment());
        frameMemory=memory(device,stride*slots,DkMemBlockFlags_GpuCached|DkMemBlockFlags_Image);
        std::array<DkImage const *,slots> images;
        for(unsigned i=0;i<slots;++i) {frameImages[i].initialize(layout,frameMemory,i*stride);images[i]=&frameImages[i];}
        diagnostic("display-native-swapchain-begin");
        swapchain=dk::SwapchainMaker{device,nativeWindow,images}.create();
        diagnostic("display-native-swapchain-ready");
        if(!swapchain) throw std::runtime_error("GPU 显示缓冲创建失败");
        code=memory(device,1<<18,sharedFlags|DkMemBlockFlags_Code);
        uint32_t offset=0;const char *names[]={"nv12.vert","nv12.frag","ui.vert","ui.frag"};
        for(unsigned i=0;i<4;++i) {
            std::ifstream file(std::string("romfs:/shaders/")+names[i]+".dksh",std::ios::binary|std::ios::ate);
            if(!file) throw std::runtime_error("缺少 GPU 显示着色器");
            const auto bytes=file.tellg();
            if(bytes<=0 || uint64_t(offset)+uint64_t(bytes)>code.getSize()) throw std::runtime_error("GPU 着色器无效");
            file.seekg(0);file.read(static_cast<char *>(code.getCpuAddr())+offset,bytes);
            if(!file) throw std::runtime_error("GPU 着色器读取失败");
            dk::ShaderMaker{code,offset}.initialize(shaders[i]);
            offset=aligned(offset+uint32_t(bytes),DK_SHADER_CODE_ALIGNMENT);
        }
        samplerMemory=memory(device,DK_MEMBLOCK_ALIGNMENT,sharedFlags);
        DkSampler sampler;dkSamplerDefaults(&sampler);
        sampler.minFilter=sampler.magFilter=DkFilter_Linear;
        for(auto &mode:sampler.wrapMode) mode=DkWrapMode_ClampToEdge;
        dkSamplerDescriptorInitialize(static_cast<DkSamplerDescriptor *>(samplerMemory.getCpuAddr()),&sampler);
        for(auto &frame:frames) {
            frame.commands=memory(device,commandBytes,sharedFlags);
            frame.uniforms=memory(device,uniformBytes,sharedFlags);
            frame.images=memory(device,descriptors*sizeof(DkImageDescriptor),sharedFlags);
            frame.command=dk::CmdBufMaker{device}.create();
            if(!frame.command) throw std::runtime_error("GPU 命令缓冲创建失败");
        }
        const uint32_t pixel=0xffffffff;white=upload(reinterpret_cast<const uint8_t *>(&pixel),1,1,4);
    }
    ~Renderer() {
        if(queue) queue.waitIdle();
        for(auto &frame:frames) {frame.textures.clear();frame.video.reset();}
        textures.clear();white.reset();cpuVideo.reset();nativeVideo.reset();nativeCache.clear();
        if(fontSurface) SDL_FreeSurface(fontSurface);
    }
    std::shared_ptr<Texture> upload(const uint8_t *data,int w,int h,int pitch) {
        if(!data || w<=0 || h<=0 || w>8192 || h>8192 || pitch<w*4) return {};
        auto texture=std::make_shared<Texture>();texture->width=w;texture->height=h;
        texture->pitch=aligned(uint32_t(w)*4,DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
        dk::ImageLayoutMaker{device}.setDimensions(w,h).setType(DkImageType_2D)
            .setFormat(DkImageFormat_RGBA8_Unorm).setFlags(DkImageFlags_PitchLinear)
            .setPitchStride(texture->pitch).initialize(texture->layout);
        texture->storage=memory(device,uint32_t(texture->layout.getSize()),sharedFlags|DkMemBlockFlags_Image);
        for(int y=0;y<h;++y) std::memcpy(static_cast<uint8_t *>(texture->storage.getCpuAddr())+y*texture->pitch,
                                       data+y*pitch,size_t(w)*4);
        texture->image.initialize(texture->layout,texture->storage,0);texture->descriptor.initialize(texture->image);
        return texture;
    }
    Slot &slot() {return frames[current];}
    void begin() {
        if(recording) return;
        current=(current+1)%slots;auto &f=slot();
        if(f.pending && f.fence.wait(1000000000LL)!=DkResult_Success) {
            // Never release surfaces or reuse command memory before completion.
            queue.waitIdle();
        }
        f.pending=false;f.textures.clear();f.video.reset();f.used=f.imageCount=0;
        imageSlot=queue.acquireImage(swapchain);
        if(imageSlot<0 || imageSlot>=int(slots)) throw std::runtime_error("GPU 显示缓冲获取失败");
        f.command.clear();f.command.addMemory(f.commands,0,commandBytes);
        dk::ImageView target{frameImages[imageSlot]};f.command.bindRenderTargets({&target});
        // Screen-space UI/video triangles have no back face. Upper-left,
        // +Y-down coordinates reverse their visual winding versus GL defaults.
        DkRasterizerState raster;dkRasterizerStateDefaults(&raster);
        raster.cullMode=DkFace_None;f.command.bindRasterizerState(raster);
        DkDepthStencilState depth;dkDepthStencilStateDefaults(&depth);f.command.bindDepthStencilState(depth);
        DkMultisampleState samples;dkMultisampleStateDefaults(&samples);f.command.bindMultisampleState(samples);
        DkColorWriteState write;dkColorWriteStateDefaults(&write);f.command.bindColorWriteState(write);
        f.command.bindImageDescriptorSet(f.images.getGpuAddr(),descriptors);
        f.command.bindSamplerDescriptorSet(samplerMemory.getGpuAddr(),1);
        DkScissor clip{0,0,uint32_t(width),uint32_t(height)};f.command.setScissors(0,{clip});
        f.command.clearColor(0,DkColorMask_RGBA,color.r/255.f,color.g/255.f,color.b/255.f,color.a/255.f);
        recording=true;
    }
    DkGpuAddr uniform(DkStage stage,const void *data,size_t bytes) {
        auto &f=slot();const auto offset=aligned(f.used,DK_UNIFORM_BUF_ALIGNMENT);
        if(bytes>DK_UNIFORM_BUF_ALIGNMENT || offset+DK_UNIFORM_BUF_ALIGNMENT>uniformBytes)
            throw std::runtime_error("GPU 单帧界面过大");
        std::memset(static_cast<uint8_t *>(f.uniforms.getCpuAddr())+offset,0,DK_UNIFORM_BUF_ALIGNMENT);
        std::memcpy(static_cast<uint8_t *>(f.uniforms.getCpuAddr())+offset,data,bytes);
        f.used=offset+DK_UNIFORM_BUF_ALIGNMENT;
        const auto address=f.uniforms.getGpuAddr()+offset;
        f.command.bindUniformBuffer(stage,0,address,DK_UNIFORM_BUF_ALIGNMENT);return address;
    }
    DkResHandle descriptor(const dk::ImageDescriptor &image) {
        auto &f=slot();if(f.imageCount>=descriptors) throw std::runtime_error("GPU 界面纹理过多");
        const auto index=f.imageCount++;
        static_cast<DkImageDescriptor *>(f.images.getCpuAddr())[index]=image;
        return dkMakeTextureHandle(index,0);
    }
    void viewport(const SDL_Rect &r) {
        const float sx=width/1280.f,sy=height/720.f;
        DkViewport v{r.x*sx,r.y*sy,r.w*sx,r.h*sy,0,1};slot().command.setViewports(0,{v});
    }
    void textured(const std::shared_ptr<Texture> &texture,const SDL_Rect &source,const SDL_Rect &dest,SDL_Color tint) {
        if(!texture || source.w<=0 || source.h<=0 || dest.w<=0 || dest.h<=0) return;
        begin();auto &f=slot();viewport(dest);
        DkShader const *programs[]={&shaders[2],&shaders[3]};dkCmdBufBindShaders(f.command,DkStageFlag_GraphicsMask,programs,2);
        const float rect[]={source.x/float(texture->width),source.y/float(texture->height),
                            source.w/float(texture->width),source.h/float(texture->height)};
        const float rgba[]={tint.r/255.f,tint.g/255.f,tint.b/255.f,tint.a/255.f};
        uniform(DkStage_Vertex,rect,sizeof rect);uniform(DkStage_Fragment,rgba,sizeof rgba);
        DkColorState state;dkColorStateDefaults(&state);dkColorStateSetBlendEnable(&state,0,true);
        f.command.bindColorState(state);
        dk::BlendState blend;blend.setFactors(DkBlendFactor_SrcAlpha,DkBlendFactor_InvSrcAlpha,DkBlendFactor_One,DkBlendFactor_InvSrcAlpha);
        f.command.bindBlendStates(0,{blend});const auto handle=descriptor(texture->descriptor);
        f.command.bindTextures(DkStage_Fragment,0,{handle});
        f.command.barrier(DkBarrier_None,DkInvalidateFlags_Image|DkInvalidateFlags_Descriptors);
        f.command.draw(DkPrimitive_Triangles,3,1,0,0);f.textures.push_back(texture);
    }
    void drawVideo(const SDL_Rect &dest) {
        if(!nativeVideo) {if(cpuVideo) textured(cpuVideo,{0,0,cpuVideo->width,cpuVideo->height},dest,{255,255,255,255});return;}
        begin();auto &f=slot();viewport(dest);
        DkShader const *programs[]={&shaders[0],&shaders[1]};dkCmdBufBindShaders(f.command,DkStageFlag_GraphicsMask,programs,2);
        uniform(DkStage_Fragment,nativeVideo->colorRows.data(),sizeof(nativeVideo->colorRows));
        DkColorState state;dkColorStateDefaults(&state);f.command.bindColorState(state);
        const DkResHandle handles[]={descriptor(nativeVideo->yDescriptor()),descriptor(nativeVideo->uvDescriptor())};
        dkCmdBufBindTextures(f.command,DkStage_Fragment,0,handles,2);
        f.command.barrier(DkBarrier_None,DkInvalidateFlags_Image|DkInvalidateFlags_Descriptors);
        f.command.draw(DkPrimitive_Triangles,3,1,0,0);f.video=nativeVideo;
    }
    void present() {
        begin();auto &f=slot();f.command.signalFence(f.fence,true);
        queue.submitCommands(f.command.finishList());queue.presentImage(swapchain,imageSlot);
        f.pending=true;recording=false;
    }
};
std::unique_ptr<Renderer> renderer;
bool requested() {
    // Native display is the default now. An explicit native_display=0 keeps
    // the SDL fallback available for troubleshooting, and holding Minus at
    // launch still bypasses native display without changing the config.
    const auto configured=ConfigStore::instance().value("native_display");
    bool enabled=configured!="0";
    PadState pad;padInitializeDefault(&pad);padUpdate(&pad);
    return enabled && !(padGetButtons(&pad)&HidNpadButton_Minus);
}
}
bool nativeRequested() {return requested();}
SDL_Renderer *createRenderer(SDL_Window *&window,int width,int height) {
    if(!window) {
        try {
            renderer=std::make_unique<Renderer>(width,height);
            renderer->fontSurface=SDL_CreateRGBSurfaceWithFormat(0,1280,720,32,SDL_PIXELFORMAT_RGBA32);
            auto *software=renderer->fontSurface?SDL_CreateSoftwareRenderer(renderer->fontSurface):nullptr;
            if(!software) throw std::runtime_error("字体缓存初始化失败");
            diagnostic("display-deko-no-cull");diagnostic("display-deko-enabled");return software;
        } catch(...) {renderer.reset();diagnosticException("display-deko-init-failed",std::current_exception());}
    }
    if(!window) {
        diagnostic("display-sdl-fallback-window");
        window=SDL_CreateWindow("UU Remote NS",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
                               width,height,SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
        if(!window) return nullptr;
    }
    auto *result=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!result) result=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    return result;
}
bool active() {return bool(renderer);}
void shutdown() {renderer.reset();}
void setColor(SDL_Renderer *r,Uint8 red,Uint8 green,Uint8 blue,Uint8 alpha) {
    if(renderer) renderer->color={red,green,blue,alpha};else SDL_SetRenderDrawColor(r,red,green,blue,alpha);
}
int fillRect(SDL_Renderer *r,const SDL_Rect *rect) {
    if(!renderer) return SDL_RenderFillRect(r,rect);
    const SDL_Rect all{0,0,1280,720};renderer->textured(renderer->white,{0,0,1,1},rect?*rect:all,renderer->color);return 0;
}
int clear(SDL_Renderer *r) {if(!renderer) return SDL_RenderClear(r);renderer->begin();return 0;}
void present(SDL_Renderer *r) {if(renderer) renderer->present();else SDL_RenderPresent(r);}
SDL_Texture *textureFromSurface(SDL_Renderer *r,SDL_Surface *surface) {
    auto *texture=SDL_CreateTextureFromSurface(r,surface);
    if(renderer && texture) {
        auto *rgba=SDL_ConvertSurfaceFormat(surface,SDL_PIXELFORMAT_RGBA32,0);
        if(rgba) {renderer->textures[texture]=renderer->upload(static_cast<uint8_t *>(rgba->pixels),rgba->w,rgba->h,rgba->pitch);SDL_FreeSurface(rgba);}
    }
    return texture;
}
void destroyTexture(SDL_Texture *texture) {if(renderer) renderer->textures.erase(texture);SDL_DestroyTexture(texture);}
int copy(SDL_Renderer *r,SDL_Texture *texture,const SDL_Rect *src,const SDL_Rect *dst) {
    if(!renderer) return SDL_RenderCopy(r,texture,src,dst);
    auto it=renderer->textures.find(texture);if(it==renderer->textures.end() || !it->second) return -1;
    const auto &image=it->second;SDL_Rect all{0,0,image->width,image->height};
    renderer->textured(image,src?*src:all,dst?*dst:all,{255,255,255,255});return 0;
}
int vsync(SDL_Renderer *r,int enabled) {
    if(!renderer) return SDL_RenderSetVSync(r,enabled);
    // Keep both the window and the deko3d swapchain interval in sync. The
    // latter is what deko3d consults when it queues/acquires present images;
    // setting only the native window was leaving 720p handheld present loops
    // unthrottled on some units (observed 120 Hz instead of 60 Hz).
    renderer->swapchain.setSwapInterval(enabled?1u:0u);
    return R_SUCCEEDED(nwindowSetSwapInterval(nwindowGetDefault(),enabled?1:0))?0:-1;
}
bool acceptVideo(const Picture &picture) {
    if(!renderer) return false;
    if(picture.hardwareFrame) {
        auto frame=NativeNv12Surface::import(renderer->device,picture.hardwareFrame.get(),renderer->nativeCache);
        if(!frame) return false;
        renderer->nativeVideo=std::move(frame);renderer->cpuVideo.reset();++renderer->hardwareFrames;return true;
    }
    if(!picture.pixels) return false;
    std::vector<uint8_t> rgba(size_t(picture.width)*picture.height*4);
    if(SDL_ConvertPixels(picture.width,picture.height,picture.pixelFormat,picture.pixels->data(),
        picture.pixelFormat==SDL_PIXELFORMAT_NV12?picture.width:picture.width*4,SDL_PIXELFORMAT_RGBA32,
        rgba.data(),picture.width*4)!=0) return false;
    auto image=renderer->upload(rgba.data(),picture.width,picture.height,picture.width*4);
    if(!image) return false;
    renderer->cpuVideo=std::move(image);renderer->nativeVideo.reset();return true;
}
void drawVideo(const SDL_Rect &rect) {if(renderer) renderer->drawVideo(rect);}
void resetVideo() {if(renderer) {renderer->nativeVideo.reset();renderer->cpuVideo.reset();renderer->nativeCache.clear();}}
uint64_t directFrames() {return renderer?renderer->hardwareFrames:0;}
uint64_t mapHits() {return renderer?renderer->nativeCache.hits():0;}
uint64_t mapImports() {return renderer?renderer->nativeCache.imports():0;}
uint64_t mapCached() {return renderer?renderer->nativeCache.size():0;}
}
#else
namespace uu::display {
bool nativeRequested(){return false;}
SDL_Renderer *createRenderer(SDL_Window *&w,int,int) {auto *r=SDL_CreateRenderer(w,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);return r?r:SDL_CreateRenderer(w,-1,SDL_RENDERER_SOFTWARE);}
bool active(){return false;} void shutdown(){}
void setColor(SDL_Renderer *r,Uint8 a,Uint8 b,Uint8 c,Uint8 d){SDL_SetRenderDrawColor(r,a,b,c,d);}
int fillRect(SDL_Renderer *r,const SDL_Rect *a){return SDL_RenderFillRect(r,a);}
int clear(SDL_Renderer *r){return SDL_RenderClear(r);} void present(SDL_Renderer *r){SDL_RenderPresent(r);}
SDL_Texture *textureFromSurface(SDL_Renderer *r,SDL_Surface *s){return SDL_CreateTextureFromSurface(r,s);}
void destroyTexture(SDL_Texture *t){SDL_DestroyTexture(t);}
int copy(SDL_Renderer *r,SDL_Texture *t,const SDL_Rect *a,const SDL_Rect *b){return SDL_RenderCopy(r,t,a,b);}
int vsync(SDL_Renderer *r,int v){return SDL_RenderSetVSync(r,v);}
bool acceptVideo(const Picture &){return false;} void drawVideo(const SDL_Rect &){} void resetVideo(){} uint64_t directFrames(){return 0;} uint64_t mapHits(){return 0;} uint64_t mapImports(){return 0;} uint64_t mapCached(){return 0;}
}
#endif
