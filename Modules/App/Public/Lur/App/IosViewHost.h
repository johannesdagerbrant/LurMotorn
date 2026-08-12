#pragma once

// The iOS UIKit half of the #73 reattach heal, written once (issue #43, Phase 3 section B).
//
// Objective-C++ only — it deals in UIKit objects, so it cannot live in Platform.h (plain C++, includable
// from a game's own .cpp). Guarded on __OBJC__ so an accidental C++ include fails loudly rather than
// dragging Apple headers somewhere they don't belong.
#if defined(__OBJC__)

#import <UIKit/UIKit.h>

// Rebuild the whole UIKit hosting chain against the live window server: pick a connected
// UIWindowScene, detach the old window, install a fresh view of `ViewClass` as `Vc.view`, configure its
// CAMetalLayer, and host it in a fresh scene-attached UIWindow made key and visible.
//
// WHY THIS EXISTS AT ALL (#73): a DVT launch can initialise the renderer while the app is not active, so
// its CAMetalLayer is bound to a window-server surface that is never composited — presents SUCCEED, the
// screen stays black, and nothing errors. Recreating the swapchain, or even the VkSurfaceKHR, against the
// SAME layer cannot fix it (proven by 898999b); only a whole new chain can. Another entry for the batch's
// running theme: the success-shaped signal (a present that returns fine) was the thing lying.
//
// Returns the fresh view, or **nil** when there is no connected UIWindowScene yet — that is not a failure,
// it is "too early", and the caller's periodic tick should simply try again. A window created without a
// scene is just another orphan, which is the bug this is healing.
//
// TWO ORDERING FIXES ARE BAKED IN HERE, both paid for on device, which is most of why this must not be
// re-derived per game:
//   1. The OLD window is detached FIRST. It still holds rootViewController == the caller, and its later
//      dealloc tears that VC's view out of whatever window hosts it BY THEN — re-unhosting the fresh view
//      and making the heal loop every 2 s.
//   2. The window is attached to an EXPLICIT UIWindowScene. -initWithFrame: relies on legacy adoption into
//      the implicit scene, which is exactly what the broken launch never does.
//
// The RENDERER half is deliberately NOT here, because the two games legitimately differ: chess owns its
// renderer on the main thread and can Shutdown/Init inline, while RPS (#183) must park its render thread
// first and hand the reinit over. The caller therefore keeps the renderer dance — and must grab the
// outgoing `Vc.view` BEFORE calling if it needs to hold that layer alive across a deferred teardown (RPS
// does: the old VkSurfaceKHR wraps it and vkDestroySurfaceKHR runs later, on the render thread).
//
// MAIN THREAD ONLY — it is UIKit.
UIView* LurRebuildViewHost(UIViewController* Vc, Class ViewClass);

// Is there a UIWindowScene to host into yet? Exactly the condition that makes LurRebuildViewHost return
// nil, exposed so a caller with expensive preparation can check BEFORE paying for it: RPS parks its render
// thread across the rebuild, and doing that only to discover there is no scene would stall rendering for
// up to a second on every retry of a heal that hasn't become possible yet. Callers must STILL handle a nil
// rebuild — the scene can go away between the two calls.
bool LurHasConnectedWindowScene();

#endif  // __OBJC__
