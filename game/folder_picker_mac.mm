// folder_picker_mac.mm -- PickFolder (launch.h) on macOS: an NSOpenPanel.

#include "launch.h"

#import <AppKit/AppKit.h>

namespace game {

PickResult PickFolder(const std::string& title, std::string* out) {
    @autoreleasepool {
        // SDL's application object, set up by ResolvePlayerSetup.
        [NSApplication sharedApplication];
        [NSApp activateIgnoringOtherApps:YES];

        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.canCreateDirectories = NO;
        panel.message = [NSString stringWithUTF8String:title.c_str()];
        panel.prompt = @"Choose";
        if ([panel runModal] != NSModalResponseOK) return PickResult::Cancelled;

        NSURL* url = panel.URLs.firstObject;
        if (url == nil || url.fileSystemRepresentation == nullptr) return PickResult::Cancelled;
        *out = url.fileSystemRepresentation;
        return PickResult::Picked;
    }
}

}  // namespace game
