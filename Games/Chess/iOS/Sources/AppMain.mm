// iOS entry point. The iOS counterpart of Games/Chess/Android/.../AndroidMain.cpp.
//
// Deliberately the thinnest possible UIKit host: a single window with one label.
// Its only jobs in this skeleton are to (1) prove the shared, perft-verified C++
// chess core compiles and runs on iOS, and (2) bring up the CoreBluetooth BLE
// backend so a later cross-platform BLE test against the Android app can run.
//
// NO renderer / Vulkan / MoltenVK yet — graphics are deferred (issue #9). All real
// logic stays in C++; this Obj-C++ shim only owns the UIKit lifecycle.
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>

#include "Chess/Board.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/BleProtocol.h"

// Run the shared chess core and return the legal-move count from the start
// position (must be 20 — the same smoke test the Android app logs). Defined as a
// free function so the logic reads identically to AndroidMain.cpp.
static int ChessCoreSmokeTest() {
    Chess::Board Board = Chess::Board::StartPosition();
    Chess::MoveList Moves;
    Chess::GenerateLegalMoves(Board, Moves);
    return Moves.Count;
}

@interface OnlyChessViewController : UIViewController
@end

@implementation OnlyChessViewController {
    Lur::Transport::ITransport* _Transport;  // owned by its translation unit (1:1 link)
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    // (1) Chess-core smoke test — proves the shared C++ core runs on iOS.
    const int MoveCount = ChessCoreSmokeTest();
    NSLog(@"OnlyChess: Chess core alive: %d legal moves from the start position", MoveCount);

    UILabel* Label = [[UILabel alloc] initWithFrame:self.view.bounds];
    Label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    Label.textAlignment = NSTextAlignmentCenter;
    Label.numberOfLines = 0;
    Label.textColor = [UIColor whiteColor];
    Label.text = [NSString stringWithFormat:@"OnlyChess\n%d legal moves\nfrom the start position", MoveCount];
    [self.view addSubview:Label];

    // (2) Bring up the BLE transport. Discovery starts immediately: the device
    // both advertises and scans, then DecideBleRole tie-breaks who hosts the GATT
    // peripheral once a LurMotorn peer is seen (see IosBleTransport.mm). We pass
    // Peripheral as an initial hint; the backend re-decides per peer.
    _Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Peripheral);

    // Log any datagram from the peer and bounce one reply back, so a live
    // Android<->iPhone link shows a round-trip in the console. Mirrors AndroidMain.
    auto* T = _Transport;
    _Transport->SetReceiver([T](const uint8_t* Data, std::size_t Size) {
        NSMutableString* Hex = [NSMutableString string];
        for (std::size_t i = 0; i < Size && i < 16; ++i) [Hex appendFormat:@"%02X ", Data[i]];
        NSLog(@"OnlyChess: BLE received %zu bytes: %@", Size, Hex);
        static bool Replied = false;
        if (!Replied) { Replied = true; const uint8_t Pong = 0x5C; T->Send(&Pong, 1); }
    });
    NSLog(@"OnlyChess: BLE transport up (advertising + scanning)");
}

@end

@interface OnlyChessAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation OnlyChessAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.rootViewController = [[OnlyChessViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil,
                                 NSStringFromClass([OnlyChessAppDelegate class]));
    }
}
