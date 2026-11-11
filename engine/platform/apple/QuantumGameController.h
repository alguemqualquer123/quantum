// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef QuantumGameController_h
#define QuantumGameController_h

#import <Foundation/Foundation.h>

// Bridges Apple's GameController framework (GCController) to the engine's
// systemGamepad* events. Shared by the iOS and macOS backends.
@interface QuantumGameController : NSObject

// Idempotent: starts observing controller connect/disconnect and registers any
// already-connected controllers. Call once after Engine::systemInit.
+ (void)start;

// Suspending lets go of held input and stops reading the pads; resuming
// reports what is still held as new input.
+ (void)setSuspended:(BOOL)suspended;

@end

#endif /* QuantumGameController_h */
