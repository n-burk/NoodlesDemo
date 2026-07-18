#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface NoodlesAppleMacDemoViewController : NSViewController
- (instancetype)initWithAssetsPath:(NSString *)assetsPath
    NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;
- (instancetype)initWithNibName:(nullable NSNibName)nibNameOrNil
                         bundle:(nullable NSBundle *)nibBundleOrNil
    NS_UNAVAILABLE;
@end

NS_ASSUME_NONNULL_END
