// macOS has no wxWindow::SetIcon-to-NSApplication bridge; the Dock and
// application switcher read NSApplication.applicationIconImage directly.
#include "app_icon.h"

#import <AppKit/AppKit.h>

extern "C" void m2_launcher_set_app_icon(const char* utf8_path) {
    if (!utf8_path || !*utf8_path) return;
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:utf8_path];
        if (!path) return;
        NSURL* url = [NSURL fileURLWithPath:path];
        NSImage* image = [[NSImage alloc] initWithContentsOfURL:url];
        if (image) {
            NSApplication* app = [NSApplication sharedApplication];
            app.applicationIconImage = image;
            NSDockTile* tile = [app dockTile];
            if (tile) {
                NSImageView* iv = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, tile.size.width, tile.size.height)];
                [iv setImage:image];
                [iv setImageScaling:NSImageScaleProportionallyUpOrDown];
                [tile setContentView:iv];
                [tile display];
            }
        }
    }
}
