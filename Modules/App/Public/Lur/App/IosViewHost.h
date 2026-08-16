#pragma once

// The iOS #73 reattach heal, written once (issue #43, Phase 3 sections B and C).
//
// Objective-C++ only — it deals in UIKit objects, so it cannot live in Platform.h (plain C++, includable
// from a game's own .cpp). Guarded on __OBJC__ so an accidental C++ include fails loudly rather than
// dragging Apple headers somewhere they don't belong.
#if defined(__OBJC__)

#import <UIKit/UIKit.h>

#include "Lur/App/RenderHandshake.h"   // the park/reinit protocol the reattach drives

// Heal a #73 detached window: rebuild the whole UIKit hosting chain against the live window server and arm
// the renderer's rebuild against the new surface.
//
// WHY THIS EXISTS AT ALL (#73): a DVT launch can initialise the renderer while the app is not active, so
// its CAMetalLayer is bound to a window-server surface that is never composited — presents SUCCEED, the
// screen stays black, and nothing errors. Recreating the swapchain, or even the VkSurfaceKHR, against the
// SAME layer cannot fix it (proven by 898999b); only a whole new chain can. Another entry for the running
// theme: the success-shaped signal (a present that returns fine) was the thing lying.
//
// The sequence, which is the ordering-sensitive part and was duplicated in both games:
//
//   1. bail early if there is no connected UIWindowScene — parking a renderer to discover that stalls
//      rendering for up to a second on every 2 s retry of a heal that is not yet possible;
//   2. park the renderer and wait until it is parked;
//   3. grab the outgoing view, so its CAMetalLayer outlives a teardown that happens later, elsewhere;
//   4. rebuild the hosting chain — fresh scene-attached UIWindow, fresh view of `ViewClass`, configured
//      CAMetalLayer;
//   5. arm the reinit against the NEW surface, then release the park.
//
// Step 2 is why this could not be shared before section C: it means "wait for an ack" under a dedicated
// render thread and "return immediately" when the caller IS the frame loop, and getting it wrong is a
// self-deadlock one way and a use-after-free the other. RenderHandshake::IsParked() decides that from the
// configured topology, so the sequence is finally one piece of code for both games.
//
// TWO ORDERING FIXES ARE BAKED INTO STEP 4, both paid for on device, which is most of why this must not be
// re-derived per game:
//   a. The OLD window is detached FIRST. It still holds rootViewController == the caller, and its later
//      dealloc tears that VC's view out of whatever window hosts it BY THEN — re-unhosting the fresh view
//      and making the heal loop every 2 s.
//   b. The window is attached to an EXPLICIT UIWindowScene. -initWithFrame: relies on legacy adoption into
//      the implicit scene, which is exactly what the broken launch never does.
//
// Returns the RETIRING view on success — hold it until the rebuild completes if your renderer tears down
// asynchronously (RPS: the old VkSurfaceKHR wraps its layer and vkDestroySurfaceKHR runs later on the
// render thread), or ignore it if it does not (chess rebuilds before returning). Returns nil when the heal
// could not proceed; that is "too early", not a failure, and the park is released before returning so
// nothing is left stalled. Never nil on success — a hosted view controller always has a view.
//
// What stays with the caller is what genuinely differs: WHO applies the reinit. Poll RH.TakeWork() on the
// render thread (RPS) or call it immediately (chess) — plus each app's own follow-up work (safe-area
// insets, the init-while-inactive flag, a render-scale reset).
//
// MAIN THREAD ONLY — it is UIKit.
UIView* LurReattachRenderHost(UIViewController* Vc, Class ViewClass, Lur::App::RenderHandshake& RH);

#endif  // __OBJC__
