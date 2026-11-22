/* Harness-only LD_PRELOAD shim. Keep the game's real GL/Vulkan window and
 * rendering path without taking desktop focus, grabbing or moving the pointer.
 * SDL2 ABI declarations keep this tiny helper independent of SDL development
 * packages; the game already provides SDL at runtime. */
#include <dlfcn.h>
#include <stdint.h>

typedef struct SDL_Window SDL_Window;
enum { SDL_WINDOW_SHOWN = 0x00000004, SDL_WINDOW_HIDDEN = 0x00000008 };

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags) {
    typedef SDL_Window* (*CreateWindow)(const char*, int, int, int, int, uint32_t);
    CreateWindow create_window = (CreateWindow)dlsym(RTLD_NEXT, "SDL_CreateWindow");
    return create_window(title, x, y, w, h, (flags & ~SDL_WINDOW_SHOWN) | SDL_WINDOW_HIDDEN);
}

void SDL_ShowWindow(SDL_Window* window) { (void)window; }
void SDL_RaiseWindow(SDL_Window* window) { (void)window; }
void SDL_SetWindowGrab(SDL_Window* window, int grabbed) { (void)window; (void)grabbed; }
int SDL_SetRelativeMouseMode(int enabled) { (void)enabled; return 0; }
int SDL_CaptureMouse(int enabled) { (void)enabled; return 0; }
int SDL_WarpMouseGlobal(int x, int y) { (void)x; (void)y; return 0; }
void SDL_WarpMouseInWindow(SDL_Window* window, int x, int y) { (void)window; (void)x; (void)y; }
