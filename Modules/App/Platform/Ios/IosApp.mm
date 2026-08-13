// The iOS entry point's ceremony (issue #43, Phase 3 section B). See IosApp.h for what this
// deliberately does NOT own — the view controller, and with it the frame loop.
#import "Lur/App/IosApp.h"

#import <QuartzCore/CAMetalLayer.h>

#include <cstdlib>

@implementation LurMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

// The root view controller class, stashed by LurIosMain. UIApplicationMain instantiates the
// delegate itself from a class name, so there is nowhere to hand it a parameter — a file-static is
// the standard way through that, and it is written once before UIKit starts and only read after.
static Class GRootViewControllerClass = nil;

@implementation LurAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.rootViewController = [[GRootViewControllerClass alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end

int LurIosMain(int Argc, char* Argv[], Class ViewControllerClass) {
    GRootViewControllerClass = ViewControllerClass;

    // #103: let vkQueueSubmit return WITHOUT blocking on Metal command-buffer scheduling. MoltenVK
    // defaults this ON, so every submit stalled a full vsync inside nextDrawable (measured
    // es.submit ~16 ms) and the CPU could never run ahead, whatever the frames-in-flight count.
    //
    // Turning it off is safe ONLY because the shared Vulkan backend pipelines N frames with
    // per-slot fences + per-image semaphores (VulkanBackend.cpp), which order the GPU work that
    // async submit no longer serializes. That backend is engine code, identical for every game, so
    // the precondition holds wherever this runs — which is exactly why the setting belongs here and
    // not in one game's main. RPS set it; chess did not, and nothing would ever have told us.
    //
    // Env-var form rather than the vk_mvk_moltenvk API, so the shared backend stays free of
    // MoltenVK headers. It must land before the first vkCreateInstance, hence before
    // UIApplicationMain rather than in a view controller.
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", /*overwrite*/ 1);

    @autoreleasepool {
        return UIApplicationMain(Argc, Argv, nil, NSStringFromClass([LurAppDelegate class]));
    }
}
