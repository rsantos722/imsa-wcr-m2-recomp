// Physical-key translation from native wxWidgets key events to SDL scancodes.
//
// Saved bindings hold SDL scancodes because m2host reads the keyboard through
// SDL. The launcher has no SDL window, so it maps each platform's raw hardware
// code to the scancode SDL itself would report for that key: the macOS virtual
// key code, the Windows set-1 scan code (lParam), or the X11/Wayland keycode
// (evdev + 8). Layout-dependent character codes are only a fallback, so a
// binding made on an AZERTY keyboard still names the key's physical position.
#pragma once

#include <SDL3/SDL.h>
#include <wx/event.h>

#include <iterator>

namespace launcher_keys {

// PC set-1 scan codes without the E0 prefix. Linux evdev codes 1-88 share
// these values.
inline SDL_Scancode set1_scancode(unsigned code) {
    static const SDL_Scancode table[] = {
        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_1, SDL_SCANCODE_2,
        SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
        SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
        SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
        SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I,
        SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A, SDL_SCANCODE_S,
        SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
        SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON,
        SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSLASH,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V,
        SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA,
        SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_KP_MULTIPLY,
        SDL_SCANCODE_LALT, SDL_SCANCODE_SPACE, SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_F1,
        SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5,
        SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9,
        SDL_SCANCODE_F10, SDL_SCANCODE_NUMLOCKCLEAR, SDL_SCANCODE_SCROLLLOCK, SDL_SCANCODE_KP_7,
        SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_4,
        SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_1,
        SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_PERIOD,
        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_F11,
        SDL_SCANCODE_F12,
    };
    return code < std::size(table) ? table[code] : SDL_SCANCODE_UNKNOWN;
}

#if defined(__WXOSX__)
// Carbon kVK_* virtual key codes, following SDL's own Cocoa table (including
// its F13-F15 -> PrintScreen/ScrollLock/Pause choice).
inline SDL_Scancode raw_scancode(const wxKeyEvent& event) {
    static const SDL_Scancode table[] = {
        SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F,
        SDL_SCANCODE_H, SDL_SCANCODE_G, SDL_SCANCODE_Z, SDL_SCANCODE_X,
        SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_B,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
        SDL_SCANCODE_Y, SDL_SCANCODE_T, SDL_SCANCODE_1, SDL_SCANCODE_2,
        SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_6, SDL_SCANCODE_5,
        SDL_SCANCODE_EQUALS, SDL_SCANCODE_9, SDL_SCANCODE_7, SDL_SCANCODE_MINUS,
        SDL_SCANCODE_8, SDL_SCANCODE_0, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_O,
        SDL_SCANCODE_U, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_I, SDL_SCANCODE_P,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_APOSTROPHE,
        SDL_SCANCODE_K, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_COMMA,
        SDL_SCANCODE_SLASH, SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_PERIOD,
        SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE, SDL_SCANCODE_GRAVE, SDL_SCANCODE_BACKSPACE,
        SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RGUI, SDL_SCANCODE_LGUI,
        SDL_SCANCODE_LSHIFT, SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_LALT, SDL_SCANCODE_LCTRL,
        SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT, SDL_SCANCODE_RCTRL, SDL_SCANCODE_RGUI,
        SDL_SCANCODE_F17, SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_KP_MULTIPLY,
        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_NUMLOCKCLEAR,
        SDL_SCANCODE_VOLUMEUP, SDL_SCANCODE_VOLUMEDOWN, SDL_SCANCODE_MUTE, SDL_SCANCODE_KP_DIVIDE,
        SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_F18,
        SDL_SCANCODE_F19, SDL_SCANCODE_KP_EQUALS, SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1,
        SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5,
        SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7, SDL_SCANCODE_F20, SDL_SCANCODE_KP_8,
        SDL_SCANCODE_KP_9, SDL_SCANCODE_INTERNATIONAL3, SDL_SCANCODE_INTERNATIONAL1, SDL_SCANCODE_KP_COMMA,
        SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F3,
        SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_LANG2, SDL_SCANCODE_F11,
        SDL_SCANCODE_LANG1, SDL_SCANCODE_PRINTSCREEN, SDL_SCANCODE_F16, SDL_SCANCODE_SCROLLLOCK,
        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_F10, SDL_SCANCODE_APPLICATION, SDL_SCANCODE_F12,
        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_PAUSE, SDL_SCANCODE_INSERT, SDL_SCANCODE_HOME,
        SDL_SCANCODE_PAGEUP, SDL_SCANCODE_DELETE, SDL_SCANCODE_F4, SDL_SCANCODE_END,
        SDL_SCANCODE_F2, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_F1, SDL_SCANCODE_LEFT,
        SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP,
    };
    const unsigned code = event.GetRawKeyCode();
    return code < std::size(table) ? table[code] : SDL_SCANCODE_UNKNOWN;
}
#elif defined(__WXMSW__)
// lParam bits 16-23 hold the scan code and bit 24 the E0 (extended) prefix.
// Pause and Num Lock share scan code 0x45, so their virtual keys decide.
inline SDL_Scancode raw_scancode(const wxKeyEvent& event) {
    const unsigned long flags = event.GetRawKeyFlags();
    const unsigned code = (flags >> 16) & 0xff;
    const bool extended = (flags >> 24) & 1;
    if (code == 0 || code == 0x45) {
        if (event.GetRawKeyCode() == 0x13) return SDL_SCANCODE_PAUSE;    // VK_PAUSE
        if (event.GetRawKeyCode() == 0x90) return SDL_SCANCODE_NUMLOCKCLEAR;  // VK_NUMLOCK
        return SDL_SCANCODE_UNKNOWN;
    }
    if (extended) {
        switch (code) {
            case 0x1c: return SDL_SCANCODE_KP_ENTER;
            case 0x1d: return SDL_SCANCODE_RCTRL;
            case 0x35: return SDL_SCANCODE_KP_DIVIDE;
            case 0x37: return SDL_SCANCODE_PRINTSCREEN;
            case 0x38: return SDL_SCANCODE_RALT;
            case 0x47: return SDL_SCANCODE_HOME;
            case 0x48: return SDL_SCANCODE_UP;
            case 0x49: return SDL_SCANCODE_PAGEUP;
            case 0x4b: return SDL_SCANCODE_LEFT;
            case 0x4d: return SDL_SCANCODE_RIGHT;
            case 0x4f: return SDL_SCANCODE_END;
            case 0x50: return SDL_SCANCODE_DOWN;
            case 0x51: return SDL_SCANCODE_PAGEDOWN;
            case 0x52: return SDL_SCANCODE_INSERT;
            case 0x53: return SDL_SCANCODE_DELETE;
            case 0x5b: return SDL_SCANCODE_LGUI;
            case 0x5c: return SDL_SCANCODE_RGUI;
            case 0x5d: return SDL_SCANCODE_APPLICATION;
            default: return SDL_SCANCODE_UNKNOWN;
        }
    }
    switch (code) {
        case 0x59: return SDL_SCANCODE_KP_EQUALS;
        case 0x70: return SDL_SCANCODE_INTERNATIONAL2;
        case 0x73: return SDL_SCANCODE_INTERNATIONAL1;
        case 0x76: return SDL_SCANCODE_F24;
        case 0x79: return SDL_SCANCODE_INTERNATIONAL4;
        case 0x7b: return SDL_SCANCODE_INTERNATIONAL5;
        case 0x7d: return SDL_SCANCODE_INTERNATIONAL3;
        case 0x7e: return SDL_SCANCODE_KP_COMMA;
        default: break;
    }
    if (code >= 0x64 && code <= 0x6e) return SDL_Scancode(SDL_SCANCODE_F13 + (code - 0x64));
    return set1_scancode(code);
}
#elif defined(__WXGTK__)
// GTK reports the hardware keycode: evdev code + 8 on both X11 and Wayland.
inline SDL_Scancode raw_scancode(const wxKeyEvent& event) {
    const unsigned long hardware = event.GetRawKeyFlags();
    if (hardware < 8) return SDL_SCANCODE_UNKNOWN;
    const unsigned code = unsigned(hardware - 8);
    switch (code) {
        case 85: return SDL_SCANCODE_LANG5;
        case 89: return SDL_SCANCODE_INTERNATIONAL1;
        case 90: return SDL_SCANCODE_LANG3;
        case 91: return SDL_SCANCODE_LANG4;
        case 92: return SDL_SCANCODE_INTERNATIONAL4;
        case 93: return SDL_SCANCODE_INTERNATIONAL2;
        case 94: return SDL_SCANCODE_INTERNATIONAL5;
        case 95: return SDL_SCANCODE_KP_COMMA;
        case 96: return SDL_SCANCODE_KP_ENTER;
        case 97: return SDL_SCANCODE_RCTRL;
        case 98: return SDL_SCANCODE_KP_DIVIDE;
        case 99: return SDL_SCANCODE_PRINTSCREEN;
        case 100: return SDL_SCANCODE_RALT;
        case 102: return SDL_SCANCODE_HOME;
        case 103: return SDL_SCANCODE_UP;
        case 104: return SDL_SCANCODE_PAGEUP;
        case 105: return SDL_SCANCODE_LEFT;
        case 106: return SDL_SCANCODE_RIGHT;
        case 107: return SDL_SCANCODE_END;
        case 108: return SDL_SCANCODE_DOWN;
        case 109: return SDL_SCANCODE_PAGEDOWN;
        case 110: return SDL_SCANCODE_INSERT;
        case 111: return SDL_SCANCODE_DELETE;
        case 113: return SDL_SCANCODE_MUTE;
        case 114: return SDL_SCANCODE_VOLUMEDOWN;
        case 115: return SDL_SCANCODE_VOLUMEUP;
        case 117: return SDL_SCANCODE_KP_EQUALS;
        case 119: return SDL_SCANCODE_PAUSE;
        case 121: return SDL_SCANCODE_KP_COMMA;
        case 125: return SDL_SCANCODE_LGUI;
        case 126: return SDL_SCANCODE_RGUI;
        case 127: return SDL_SCANCODE_APPLICATION;
        default: break;
    }
    if (code >= 183 && code <= 194) return SDL_Scancode(SDL_SCANCODE_F13 + (code - 183));
    return set1_scancode(code);
}
#else
inline SDL_Scancode raw_scancode(const wxKeyEvent&) { return SDL_SCANCODE_UNKNOWN; }
#endif

// Layout-dependent fallback for a port whose raw code is unavailable.
inline SDL_Scancode keycode_scancode(int key) {
    if (key >= 'A' && key <= 'Z') return SDL_Scancode(SDL_SCANCODE_A + (key - 'A'));
    if (key >= 'a' && key <= 'z') return SDL_Scancode(SDL_SCANCODE_A + (key - 'a'));
    if (key >= '1' && key <= '9') return SDL_Scancode(SDL_SCANCODE_1 + (key - '1'));
    if (key >= WXK_F1 && key <= WXK_F12) return SDL_Scancode(SDL_SCANCODE_F1 + (key - WXK_F1));
    switch (key) {
        case '0': return SDL_SCANCODE_0;
        case ' ': return SDL_SCANCODE_SPACE;
        case WXK_RETURN: return SDL_SCANCODE_RETURN;
        case WXK_ESCAPE: return SDL_SCANCODE_ESCAPE;
        case WXK_BACK: return SDL_SCANCODE_BACKSPACE;
        case WXK_TAB: return SDL_SCANCODE_TAB;
        case WXK_SHIFT: return SDL_SCANCODE_LSHIFT;
        case WXK_ALT: return SDL_SCANCODE_LALT;
        case WXK_CONTROL: return SDL_SCANCODE_LCTRL;
        case WXK_LEFT: return SDL_SCANCODE_LEFT;
        case WXK_RIGHT: return SDL_SCANCODE_RIGHT;
        case WXK_UP: return SDL_SCANCODE_UP;
        case WXK_DOWN: return SDL_SCANCODE_DOWN;
        case WXK_HOME: return SDL_SCANCODE_HOME;
        case WXK_END: return SDL_SCANCODE_END;
        case WXK_PAGEUP: return SDL_SCANCODE_PAGEUP;
        case WXK_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
        case WXK_INSERT: return SDL_SCANCODE_INSERT;
        case WXK_DELETE: return SDL_SCANCODE_DELETE;
        default: return SDL_SCANCODE_UNKNOWN;
    }
}

// wxOSX reports a lone modifier as down or up from the combined Shift/Ctrl/
// Alt state, so releasing one Shift while the other is held reads as another
// press. Cocoa's device-dependent modifier bits (raw flags) name each side.
// Returns -1 when the key is not a modifier or the port reports both sides.
inline int modifier_down(const wxKeyEvent& event, SDL_Scancode code) {
#if defined(__WXOSX__)
    unsigned long bit = 0;
    switch (code) {
        case SDL_SCANCODE_LCTRL: bit = 0x0001; break;
        case SDL_SCANCODE_LSHIFT: bit = 0x0002; break;
        case SDL_SCANCODE_RSHIFT: bit = 0x0004; break;
        case SDL_SCANCODE_LGUI: bit = 0x0008; break;
        case SDL_SCANCODE_RGUI: bit = 0x0010; break;
        case SDL_SCANCODE_LALT: bit = 0x0020; break;
        case SDL_SCANCODE_RALT: bit = 0x0040; break;
        case SDL_SCANCODE_RCTRL: bit = 0x2000; break;
        default: return -1;
    }
    return (event.GetRawKeyFlags() & bit) ? 1 : 0;
#else
    (void)event;
    (void)code;
    return -1;
#endif
}

inline SDL_Scancode scancode(const wxKeyEvent& event) {
    const SDL_Scancode raw = raw_scancode(event);
    return raw != SDL_SCANCODE_UNKNOWN ? raw : keycode_scancode(event.GetKeyCode());
}

}  // namespace launcher_keys
