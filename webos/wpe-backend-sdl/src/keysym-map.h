#pragma once

#include <SDL/SDL_keysym.h>
#include <stdint.h>

/*
 * Map SDL 1.2 key codes to WPE key codes (XKB keysyms).
 *
 * wpe/keysyms.h already defines WPE_KEY_* constants; we include it via
 * wpe/wpe.h.  The helper functions here do the SDL → WPE translation.
 */

/* For printable ASCII, XKB keysym == Unicode codepoint */

/* WPE modifier flags */
#define WPE_MOD_SHIFT   (1 << 0)
#define WPE_MOD_CTRL    (1 << 1)
#define WPE_MOD_ALT     (1 << 2)
#define WPE_MOD_META    (1 << 3)

static inline uint32_t
sdlkey_to_wpe(SDLKey key)
{
    /* Printable ASCII maps 1:1 with XKB keysyms */
    if (key >= 0x20 && key <= 0x7E)
        return (uint32_t)key;

    switch (key) {
    case SDLK_BACKSPACE:  return 0xFF08; /* XKB_KEY_BackSpace */
    case SDLK_TAB:        return 0xFF09; /* XKB_KEY_Tab       */
    case SDLK_RETURN:     return 0xFF0D; /* XKB_KEY_Return    */
    case SDLK_ESCAPE:     return 0xFF1B; /* XKB_KEY_Escape    */
    case SDLK_DELETE:     return 0xFFFF; /* XKB_KEY_Delete    */
    case SDLK_HOME:       return 0xFF50;
    case SDLK_LEFT:       return 0xFF51;
    case SDLK_UP:         return 0xFF52;
    case SDLK_RIGHT:      return 0xFF53;
    case SDLK_DOWN:       return 0xFF54;
    case SDLK_END:        return 0xFF57;
    case SDLK_PAGEUP:     return 0xFF55;
    case SDLK_PAGEDOWN:   return 0xFF56;
    case SDLK_F1:         return 0xFFBE;
    case SDLK_F2:         return 0xFFBF;
    case SDLK_F3:         return 0xFFC0;
    case SDLK_F4:         return 0xFFC1;
    case SDLK_F5:         return 0xFFC2;
    default:              return 0;
    }
}

static inline uint32_t
sdl_mod_to_wpe(SDLMod mod)
{
    uint32_t m = 0;
    if (mod & (KMOD_LSHIFT | KMOD_RSHIFT)) m |= WPE_MOD_SHIFT;
    if (mod & (KMOD_LCTRL  | KMOD_RCTRL))  m |= WPE_MOD_CTRL;
    if (mod & (KMOD_LALT   | KMOD_RALT))   m |= WPE_MOD_ALT;
    if (mod & (KMOD_LMETA  | KMOD_RMETA))  m |= WPE_MOD_META;
    return m;
}
