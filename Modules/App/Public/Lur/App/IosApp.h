#pragma once

// The iOS entry point's ceremony, written ONCE (issue #43, Phase 3 section B). The UIKit sibling of
// AndroidApp.h, and it absorbs a smaller but even more literal duplication: everything below was
// byte-identical in the two iOS mains apart from the class names.
//
// Objective-C++ only — it deals in UIKit classes, so it lives here rather than in Platform.h (plain
// C++, includable from a game's own .cpp). Guarded on __OBJC__ so an accidental C++ include fails
// loudly instead of dragging Apple headers somewhere they don't belong. Same rule as IosViewHost.h.
//
// WHAT IT DELIBERATELY DOES NOT OWN: the view controller, and therefore the frame loop. The two
// games' loops are genuinely different — chess drives a CADisplayLink on the main thread, RPS runs
// a free-running render thread on a raw pthread with FIFO as its only vsync clock (#183, and it
// needs a 4 MB stack that std::thread cannot give it). Each game passes its own UIViewController
// class in; the engine supplies everything around it.
#if defined(__OBJC__)

#import <UIKit/UIKit.h>

// The Metal-backed view: its backing layer is a CAMetalLayer, which MoltenVK turns into a Vulkan
// surface. Both games had declared their own, identical down to the comment. Also the class to hand
// LurRebuildViewHost when healing a #73 detached window.
@interface LurMetalView : UIView
@end

// The application delegate. Its whole job is window + rootViewController + makeKeyAndVisible, and it
// holds the `window` property that the #73 reattach replaces wholesale (the old UIWindow can be
// bound to a dead window-server surface, so healing means a FRESH one).
@interface LurAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

// The whole of main(). Pass the game's UIViewController class; it is instantiated as the root.
//
// This exists as a function rather than a documented four-line recipe because of what it does
// BEFORE UIApplicationMain — see MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS in the implementation. That
// setting has to land before the first vkCreateInstance, RPS learned it the expensive way in #103,
// and chess never got it. A pre-instance configuration step that only one of two apps performs is
// the same shape as the engine log sink that only one of them installed: invisible, and paid for in
// a diagnosis later.
int LurIosMain(int Argc, char* Argv[], Class ViewControllerClass);

#endif  // __OBJC__
