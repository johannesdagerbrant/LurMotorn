// The iOS UIKit hosting-chain rebuild — the shared half of the #73 heal (issue #43, Phase 3 section B).
// See IosViewHost.h for why this exists and which two ordering fixes are load-bearing.
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

} // namespace

bool LurHasConnectedWindowScene() { return PickScene() != nil; }

UIView* LurRebuildViewHost(UIViewController* Vc, Class ViewClass) {
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
               "LurRebuildViewHost: ViewClass's +layerClass must be CAMetalLayer");
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
