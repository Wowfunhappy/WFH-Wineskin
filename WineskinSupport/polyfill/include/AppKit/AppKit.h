/*
 * AppKit with the names the 10.12 SDK gives existing constants. The 10.12 SDK renamed them; the values,
 * and 10.9's meaning of each, are unchanged.
 */
#include_next <AppKit/AppKit.h>

#if !defined(WINESKIN_APPKIT_H) && MAC_OS_X_VERSION_MAX_ALLOWED < 101200
#define WINESKIN_APPKIT_H

/* NSEventType (10.12 names) */
#define NSEventTypeLeftMouseDown NSLeftMouseDown
#define NSEventTypeLeftMouseUp NSLeftMouseUp
#define NSEventTypeRightMouseDown NSRightMouseDown
#define NSEventTypeRightMouseUp NSRightMouseUp
#define NSEventTypeMouseMoved NSMouseMoved
#define NSEventTypeLeftMouseDragged NSLeftMouseDragged
#define NSEventTypeRightMouseDragged NSRightMouseDragged
#define NSEventTypeMouseEntered NSMouseEntered
#define NSEventTypeMouseExited NSMouseExited
#define NSEventTypeKeyDown NSKeyDown
#define NSEventTypeKeyUp NSKeyUp
#define NSEventTypeFlagsChanged NSFlagsChanged
#define NSEventTypeAppKitDefined NSAppKitDefined
#define NSEventTypeSystemDefined NSSystemDefined
#define NSEventTypeApplicationDefined NSApplicationDefined
#define NSEventTypePeriodic NSPeriodic
#define NSEventTypeCursorUpdate NSCursorUpdate
#define NSEventTypeScrollWheel NSScrollWheel
#define NSEventTypeTabletPoint NSTabletPoint
#define NSEventTypeTabletProximity NSTabletProximity
#define NSEventTypeOtherMouseDown NSOtherMouseDown
#define NSEventTypeOtherMouseUp NSOtherMouseUp
#define NSEventTypeOtherMouseDragged NSOtherMouseDragged

/* NSEventMask (10.12 names) */
typedef unsigned long long NSEventMask;
#define NSEventMaskLeftMouseDown NSLeftMouseDownMask
#define NSEventMaskLeftMouseUp NSLeftMouseUpMask
#define NSEventMaskRightMouseDown NSRightMouseDownMask
#define NSEventMaskRightMouseUp NSRightMouseUpMask
#define NSEventMaskMouseMoved NSMouseMovedMask
#define NSEventMaskLeftMouseDragged NSLeftMouseDraggedMask
#define NSEventMaskRightMouseDragged NSRightMouseDraggedMask
#define NSEventMaskMouseEntered NSMouseEnteredMask
#define NSEventMaskMouseExited NSMouseExitedMask
#define NSEventMaskKeyDown NSKeyDownMask
#define NSEventMaskKeyUp NSKeyUpMask
#define NSEventMaskFlagsChanged NSFlagsChangedMask
#define NSEventMaskAppKitDefined NSAppKitDefinedMask
#define NSEventMaskSystemDefined NSSystemDefinedMask
#define NSEventMaskApplicationDefined NSApplicationDefinedMask
#define NSEventMaskPeriodic NSPeriodicMask
#define NSEventMaskCursorUpdate NSCursorUpdateMask
#define NSEventMaskScrollWheel NSScrollWheelMask
#define NSEventMaskTabletPoint NSTabletPointMask
#define NSEventMaskTabletProximity NSTabletProximityMask
#define NSEventMaskOtherMouseDown NSOtherMouseDownMask
#define NSEventMaskOtherMouseUp NSOtherMouseUpMask
#define NSEventMaskOtherMouseDragged NSOtherMouseDraggedMask
#define NSEventMaskAny NSAnyEventMask

/* NSEventModifierFlags (10.12 names) */
typedef NSUInteger NSEventModifierFlags;
#define NSEventModifierFlagCapsLock NSAlphaShiftKeyMask
#define NSEventModifierFlagShift NSShiftKeyMask
#define NSEventModifierFlagControl NSControlKeyMask
#define NSEventModifierFlagOption NSAlternateKeyMask
#define NSEventModifierFlagCommand NSCommandKeyMask
#define NSEventModifierFlagNumericPad NSNumericPadKeyMask
#define NSEventModifierFlagHelp NSHelpKeyMask
#define NSEventModifierFlagFunction NSFunctionKeyMask
#define NSEventModifierFlagDeviceIndependentFlagsMask NSDeviceIndependentModifierFlagsMask

/* NSWindowStyleMask (10.12 names) */
typedef NSUInteger NSWindowStyleMask;
#define NSWindowStyleMaskBorderless NSBorderlessWindowMask
#define NSWindowStyleMaskTitled NSTitledWindowMask
#define NSWindowStyleMaskClosable NSClosableWindowMask
#define NSWindowStyleMaskMiniaturizable NSMiniaturizableWindowMask
#define NSWindowStyleMaskResizable NSResizableWindowMask
#define NSWindowStyleMaskTexturedBackground NSTexturedBackgroundWindowMask
#define NSWindowStyleMaskUnifiedTitleAndToolbar NSUnifiedTitleAndToolbarWindowMask
#define NSWindowStyleMaskFullScreen NSFullScreenWindowMask
#define NSWindowStyleMaskUtilityWindow NSUtilityWindowMask
#define NSWindowStyleMaskDocModalWindow NSDocModalWindowMask
#define NSWindowStyleMaskNonactivatingPanel NSNonactivatingPanelMask
#define NSWindowStyleMaskHUDWindow NSHUDWindowMask

/* NSCompositingOperation (10.12 names) */
#define NSCompositingOperationClear NSCompositeClear
#define NSCompositingOperationCopy NSCompositeCopy
#define NSCompositingOperationSourceOver NSCompositeSourceOver
#define NSCompositingOperationSourceIn NSCompositeSourceIn
#define NSCompositingOperationSourceOut NSCompositeSourceOut
#define NSCompositingOperationSourceAtop NSCompositeSourceAtop
#define NSCompositingOperationDestinationOver NSCompositeDestinationOver
#define NSCompositingOperationDestinationIn NSCompositeDestinationIn
#define NSCompositingOperationDestinationOut NSCompositeDestinationOut
#define NSCompositingOperationDestinationAtop NSCompositeDestinationAtop
#define NSCompositingOperationXOR NSCompositeXOR
#define NSCompositingOperationPlusDarker NSCompositePlusDarker
#define NSCompositingOperationHighlight NSCompositeHighlight
#define NSCompositingOperationPlusLighter NSCompositePlusLighter

/* NSAlertStyle, NSControlSize, NSButtonType (10.12 names) */
#define NSAlertStyleWarning NSWarningAlertStyle
#define NSAlertStyleInformational NSInformationalAlertStyle
#define NSAlertStyleCritical NSCriticalAlertStyle
#define NSControlSizeRegular NSRegularControlSize
#define NSControlSizeSmall NSSmallControlSize
#define NSControlSizeMini NSMiniControlSize
#define NSButtonTypeMomentaryLight NSMomentaryLightButton
#define NSButtonTypePushOnPushOff NSPushOnPushOffButton
#define NSButtonTypeToggle NSToggleButton
#define NSButtonTypeSwitch NSSwitchButton
#define NSButtonTypeRadio NSRadioButton
#define NSButtonTypeMomentaryChange NSMomentaryChangeButton
#define NSButtonTypeOnOff NSOnOffButton
#define NSButtonTypeMomentaryPushIn NSMomentaryPushInButton
#define NSControlStateValueOn NSOnState
#define NSControlStateValueOff NSOffState
#define NSControlStateValueMixed NSMixedState

/* NSBitmapImageFileType (10.14 names) */
#define NSBitmapImageFileTypeTIFF NSTIFFFileType
#define NSBitmapImageFileTypeBMP NSBMPFileType
#define NSBitmapImageFileTypeGIF NSGIFFileType
#define NSBitmapImageFileTypeJPEG NSJPEGFileType
#define NSBitmapImageFileTypePNG NSPNGFileType
#define NSBitmapImageFileTypeJPEG2000 NSJPEG2000FileType

/* NSViewLayerContentScaleDelegate (10.14 SDK): a protocol 10.7's AppKit already consults. */
@protocol NSViewLayerContentScaleDelegate <NSObject>
@optional
- (BOOL)layer:(CALayer *)layer shouldInheritContentsScale:(CGFloat)newScale fromWindow:(NSWindow *)window;
@end

/* -[NSScreen maximumPotentialExtendedDynamicRangeColorComponentValue] (10.15): 10.9 displays have no
   extended dynamic range (src/objc/AppKit.m). */
@interface NSScreen (WineskinExtendedDynamicRange)
@property (readonly) CGFloat maximumPotentialExtendedDynamicRangeColorComponentValue;
@end

/* -[NSGraphicsContext CGContext] (10.10): the context's graphicsPort, typed (src/objc/AppKit.m). */
@interface NSGraphicsContext (WineskinCGContext)
@property (readonly) CGContextRef CGContext;
@end

/* +[NSWindow allowsAutomaticWindowTabbing] (10.12): 10.9 has no window tabbing (src/objc/AppKit.m). */
@interface NSWindow (WineskinWindowTabbing)
+ (BOOL)allowsAutomaticWindowTabbing;
+ (void)setAllowsAutomaticWindowTabbing:(BOOL)allows;
@end

extern const char wineskin_appkit_polyfill;
static const char *const wineskin_appkit_polyfill_ref __attribute__((used)) = &wineskin_appkit_polyfill;

#endif
