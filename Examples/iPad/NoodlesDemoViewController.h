#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface NoodlesDemoViewController : UIViewController
- (instancetype)initWithAssetsPath:(NSString *)assetsPath
    NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;
- (instancetype)initWithNibName:(nullable NSString *)nibNameOrNil
                         bundle:(nullable NSBundle *)nibBundleOrNil
    NS_UNAVAILABLE;
@end

NS_ASSUME_NONNULL_END
