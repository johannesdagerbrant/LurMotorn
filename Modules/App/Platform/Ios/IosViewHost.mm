// The iOS #73 reattach heal (issue #43, Phase 3 sections B and C). See IosViewHost.h for why it exists,
// the five steps, and which ordering fixes are load-bearing.
#include "Lur/App/IosViewHost.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <os/log.h>

#include "Lur/Core/Assert.h"
#include "Lur/Core/LogTag.h"

namespace {

// Prefer a foreground-active scene, but take any connected UIWindowScene: during the broken launch the
// scene can exist while still inactive, and hosting there is what lets the heal complete at all.
UIWindowScene* PickScene() {
    UIWindowScene* Scene = nil;
    for (UIScene* S in UIApplication.sharedApplication.connectedScenes) {
        if (![S isKindOfClass:UIWindowScene.class]) continue;
        Scene = (UIWindowScene*)S;
        if (S.activationState == UISceneActivationStateForegroundActive) break;  // best pick
    }
    return Scene;
}

// Both of the steps below were public until section C. Nothing outside this file calls them now that
// LurReattachRenderHost owns the whole sequence, so they are internal — a public entry point with no
// caller reads as covered ground and is exactly what the repo's delete-dead-code rule is about.
bool HasConnectedWindowScene() { return PickScene() != nil; }

// Step 4 of the heal: the pure UIKit chain rebuild. The two ordering fixes below are load-bearing and
// are documented in IosViewHost.h.
UIView* RebuildViewHost(UIViewController* Vc, Class ViewClass) {
    UIWindowScene* Scene = PickScene();
    if (Scene == nil) {
        // Not a failure — too early. The caller's tick retries.
        os_log(OS_LOG_DEFAULT,
               "%{public}s: #73 reattach SKIPPED: no connected UIWindowScene (scenes=%lu) - will retry",
               Lur::Core::LogTag,
               (unsigned long)UIApplication.sharedApplication.connectedScenes.count);
        return nil;
    }
    os_log(OS_LOG_DEFAULT,
           "%{public}s: #73 reattach: view unhosted - rebuilding window+view+layer on scene state=%ld",
           Lur::Core::LogTag, (long)Scene.activationState);

    // Ordering fix 1: detach the OLD window before installing anything new (see the header).
    // `window` is an @optional property on UIApplicationDelegate, so reach it through the protocol rather
    // than a per-game delegate class — that was the only reason this code needed to know the game's types.
    id<UIApplicationDelegate> Delegate = UIApplication.sharedApplication.delegate;
    const bool HasWindowProperty = [Delegate respondsToSelector:@selector(setWindow:)] &&
                                   [Delegate respondsToSelector:@selector(window)];
    if (HasWindowProperty) {
        UIWindow* Old = Delegate.window;
        Old.hidden = YES;
        Old.rootViewController = nil;
    } else {
        // Both our delegates declare it; say so loudly rather than healing into a window nothing retains.
        os_log_error(OS_LOG_DEFAULT,
                     "%{public}s: #73 reattach: app delegate has no `window` property — the rebuilt "
                     "window will not be retained by the delegate",
                     Lur::Core::LogTag);
    }

    UIView* NewView = [[ViewClass alloc] initWithFrame:UIScreen.mainScreen.bounds];
    Vc.view = NewView;
    // ViewClass must be a UIView subclass whose +layerClass is CAMetalLayer — MoltenVK needs that layer to
    // make a VkSurfaceKHR. Trap rather than cast blindly: the wrong class here yields a plain CALayer, and
    // every line below would "succeed" against it while the app renders nothing (the #73 symptom itself).
    LUR_ASSERT([NewView.layer isKindOfClass:CAMetalLayer.class] &&
               "RebuildViewHost: ViewClass's +layerClass must be CAMetalLayer");
    CAMetalLayer* Layer = (CAMetalLayer*)NewView.layer;
    Layer.device = MTLCreateSystemDefaultDevice();
    Layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    Layer.contentsScale = UIScreen.mainScreen.scale;
    Layer.drawableSize = CGSizeMake(NewView.bounds.size.width * Layer.contentsScale,
                                    NewView.bounds.size.height * Layer.contentsScale);

    // Ordering fix 2: an EXPLICIT scene-attached window (see the header).
    UIWindow* NewWindow = [[UIWindow alloc] initWithWindowScene:Scene];
    NewWindow.frame = Scene.coordinateSpace.bounds;
    NewWindow.rootViewController = Vc;
    [NewWindow makeKeyAndVisible];
    if (HasWindowProperty) Delegate.window = NewWindow;

    return NewView;
}

} // namespace

UIView* LurReattachRenderHost(UIViewController* Vc, Class ViewClass, Lur::App::RenderHandshake& RH) {
    // Step 1 — cheap check first. RebuildViewHost would also return nil, but only after we had paid to
    // park the renderer, and under a dedicated render thread that is up to a second of stalled frames on
    // every retry of a heal that cannot succeed yet.
    if (!HasConnectedWindowScene()) {
        os_log(OS_LOG_DEFAULT, "%{public}s: #73 reattach SKIPPED: no connected UIWindowScene - will retry",
               Lur::Core::LogTag);
        return nil;
    }

    // Step 2 — park. Bounded busy-wait: this is a rare heal, not the hot loop, and the ack normally lands
    // within one frame (~16 ms). Under Inline the predicate is true on the first test and this costs
    // nothing. Proceeding un-parked is deliberately better than wedging the heal — the renderer keeps
    // drawing into a layer we are about to replace for a few more frames, which is survivable, whereas a
    // heal that never runs leaves a permanently black screen.
    RH.RequestPark();
    for (int I = 0; I < 250 && !RH.IsParked(); ++I) [NSThread sleepForTimeInterval:0.004];  // ~1 s cap
    if (!RH.IsParked())
        os_log_error(OS_LOG_DEFAULT,
                     "%{public}s: #73 reattach: renderer did not park in time - proceeding",
                     Lur::Core::LogTag);

    // Step 3 — grab the outgoing view BEFORE the rebuild reassigns Vc.view. Its CAMetalLayer is what the
    // old VkSurfaceKHR wraps, and vkDestroySurfaceKHR may not run until later, on another thread.
    UIView* Retiring = Vc.view;

    // Step 4 — the UIKit chain.
    UIView* NewView = RebuildViewHost(Vc, ViewClass);
    if (NewView == nil) {   // the scene went away between the check and here — un-park and let the tick retry
        RH.Resume();
        return nil;
    }

    // Step 5 — arm the reinit against the NEW surface, then release. Order is load-bearing and the
    // handshake enforces it: while the park stands, TakeWork withholds the reinit, so a render thread
    // cannot wake early and rebuild against the layer being replaced.
    CAMetalLayer* Layer = (CAMetalLayer*)NewView.layer;
    RH.RequestReattach((__bridge void*)Layer, static_cast<int>(Layer.drawableSize.width),
                       static_cast<int>(Layer.drawableSize.height));
    RH.Resume();
    return Retiring;
}
