#import "ViewController.h"
#import "app_info.h"

@implementation ViewController

#if TARGET_OS_IOS

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    UIScrollView *scrollView = [[UIScrollView alloc] init];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scrollView];

    // Container fills the scroll view and expands to at least the frame height,
    // so the stack stays centered when the content is shorter than the screen.
    UIView *container = [[UIView alloc] init];
    container.translatesAutoresizingMaskIntoConstraints = NO;
    [scrollView addSubview:container];

    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [container addSubview:stack];

    // Product name
    UILabel *nameLabel = [[UILabel alloc] init];
    nameLabel.text = @(APP_NAME);
    nameLabel.font = [UIFont boldSystemFontOfSize:26];
    [stack addArrangedSubview:nameLabel];

    // Version
    UILabel *versionLabel = [[UILabel alloc] init];
    versionLabel.text = [NSString stringWithFormat:@"Version %s", APP_VERSION];
    versionLabel.font = [UIFont systemFontOfSize:15];
    versionLabel.textColor = [UIColor secondaryLabelColor];
    [stack addArrangedSubview:versionLabel];

    // Description (optional)
    NSString *desc = @(APP_DESCRIPTION);
    if (desc.length > 0) {
        UILabel *descLabel = [[UILabel alloc] init];
        descLabel.text = desc;
        descLabel.font = [UIFont systemFontOfSize:15];
        descLabel.textColor = [UIColor secondaryLabelColor];
        descLabel.numberOfLines = 0;
        descLabel.textAlignment = NSTextAlignmentCenter;
        descLabel.translatesAutoresizingMaskIntoConstraints = NO;
        [stack addArrangedSubview:descLabel];
        [descLabel.widthAnchor constraintLessThanOrEqualToConstant:340].active = YES;
    }

    // Spacer
    UIView *spacer = [[UIView alloc] init];
    spacer.translatesAutoresizingMaskIntoConstraints = NO;
    [stack addArrangedSubview:spacer];
    [spacer.heightAnchor constraintEqualToConstant:8].active = YES;

    // Links
    [self addLinkButton:@"Visit Site" selector:@selector(openWebsite) toStack:stack];
    if (@(APP_MANUAL_URL).length > 0)
        [self addLinkButton:@"Read Manual" selector:@selector(openManual) toStack:stack];
    if (@(APP_SUPPORT_URL).length > 0)
        [self addLinkButton:@"Contact Support" selector:@selector(openSupport) toStack:stack];
    if (@(APP_PRIVACY_URL).length > 0)
        [self addLinkButton:@"Privacy Policy" selector:@selector(openPrivacy) toStack:stack];
    if (@(APP_STORE_URL).length > 0)
        [self addLinkButton:@"Rate on App Store" selector:@selector(openStore) toStack:stack];

    [NSLayoutConstraint activateConstraints:@[
        // Scroll view fills safe area
        [scrollView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scrollView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],

        // Container spans scroll content area, width matches frame
        [container.topAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.topAnchor],
        [container.leadingAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.leadingAnchor],
        [container.trailingAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.trailingAnchor],
        [container.bottomAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.bottomAnchor],
        [container.widthAnchor constraintEqualToAnchor:scrollView.frameLayoutGuide.widthAnchor],
        // Container is at least as tall as the visible area so content stays centered
        [container.heightAnchor constraintGreaterThanOrEqualToAnchor:scrollView.frameLayoutGuide.heightAnchor],

        // Stack centered in container, with padding guards
        [stack.centerXAnchor constraintEqualToAnchor:container.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:container.centerYAnchor],
        [stack.topAnchor constraintGreaterThanOrEqualToAnchor:container.topAnchor constant:48],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:container.bottomAnchor constant:-48],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:container.leadingAnchor constant:24],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:container.trailingAnchor constant:-24],
    ]];
}

- (void)addLinkButton:(NSString *)title selector:(SEL)selector toStack:(UIStackView *)stack {
    UIButton *btn = [UIButton buttonWithType:UIButtonTypeSystem];
    [btn setTitle:title forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont systemFontOfSize:15];
    [btn addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:btn];
}

- (void)openURLString:(NSString *)urlString {
    NSURL *url = [NSURL URLWithString:urlString];
    if (url) {
        [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
    }
}

- (void)openWebsite { [self openURLString:@(APP_COMPANY_WEBSITE)]; }
- (void)openManual   { [self openURLString:@(APP_MANUAL_URL)]; }
- (void)openSupport  { [self openURLString:@(APP_SUPPORT_URL)]; }
- (void)openPrivacy  { [self openURLString:@(APP_PRIVACY_URL)]; }
- (void)openStore    { [self openURLString:@(APP_STORE_URL)]; }

#elif TARGET_OS_OSX

- (void)loadView {
    self.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 480)];
    self.view.wantsLayer = YES;

    NSStackView *stack = [[NSStackView alloc] init];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];

    // Product name
    NSTextField *nameLabel = [NSTextField labelWithString:@(APP_NAME)];
    nameLabel.font = [NSFont boldSystemFontOfSize:22];
    [stack addArrangedSubview:nameLabel];

    // Version
    NSString *version = [NSString stringWithFormat:@"Version %s", APP_VERSION];
    NSTextField *versionLabel = [NSTextField labelWithString:version];
    versionLabel.font = [NSFont systemFontOfSize:13];
    versionLabel.textColor = [NSColor secondaryLabelColor];
    [stack addArrangedSubview:versionLabel];

    // Description (optional)
    NSString *desc = @(APP_DESCRIPTION);
    if (desc.length > 0) {
        NSTextField *descLabel = [NSTextField wrappingLabelWithString:desc];
        descLabel.font = [NSFont systemFontOfSize:13];
        descLabel.textColor = [NSColor secondaryLabelColor];
        descLabel.alignment = NSTextAlignmentCenter;
        descLabel.translatesAutoresizingMaskIntoConstraints = NO;
        [stack addArrangedSubview:descLabel];
        [descLabel.widthAnchor constraintLessThanOrEqualToConstant:420].active = YES;
    }

    // Spacer
    NSView *spacer = [[NSView alloc] init];
    spacer.translatesAutoresizingMaskIntoConstraints = NO;
    [stack addArrangedSubview:spacer];
    [spacer.heightAnchor constraintEqualToConstant:4].active = YES;

    // Links
    [self addLinkButton:@"Visit Site" selector:@selector(openWebsite) toStack:stack];
    if (@(APP_MANUAL_URL).length > 0)
        [self addLinkButton:@"Read Manual" selector:@selector(openManual) toStack:stack];
    if (@(APP_SUPPORT_URL).length > 0)
        [self addLinkButton:@"Contact Support" selector:@selector(openSupport) toStack:stack];
    if (@(APP_PRIVACY_URL).length > 0)
        [self addLinkButton:@"Privacy Policy" selector:@selector(openPrivacy) toStack:stack];
    if (@(APP_STORE_URL).length > 0)
        [self addLinkButton:@"Rate on App Store" selector:@selector(openStore) toStack:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
    ]];
}

- (void)addLinkButton:(NSString *)title selector:(SEL)selector toStack:(NSStackView *)stack {
    NSButton *btn = [[NSButton alloc] init];
    btn.bordered = NO;
    btn.target = self;
    btn.action = selector;
    NSDictionary *attrs = @{
        NSForegroundColorAttributeName: [NSColor linkColor],
        NSFontAttributeName: [NSFont systemFontOfSize:13]
    };
    btn.attributedTitle = [[NSAttributedString alloc] initWithString:title attributes:attrs];
    [stack addArrangedSubview:btn];
}

- (void)openURLString:(NSString *)urlString {
    NSURL *url = [NSURL URLWithString:urlString];
    if (url) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

- (void)openWebsite { [self openURLString:@(APP_COMPANY_WEBSITE)]; }
- (void)openManual   { [self openURLString:@(APP_MANUAL_URL)]; }
- (void)openSupport  { [self openURLString:@(APP_SUPPORT_URL)]; }
- (void)openPrivacy  { [self openURLString:@(APP_PRIVACY_URL)]; }
- (void)openStore    { [self openURLString:@(APP_STORE_URL)]; }

#endif

@end
