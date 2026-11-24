// 弱链接[ios]：S2A_SaveTransfer（C# 存档迁移入口；未链服务层时向用户报错）
// 弱链接[ios]：S2A_SaveTransferFree（释放 C# 存档迁移返回的字符串）
#import "save_transfer.h"
#import "picker_present.h"   // 选文件界面弹出 + 日志（#1939）
#import "app_settings.h"   // s2a_top_presenter
#import "s2a_l10n.h"
#import "settings.h"
#import "play_layout.h"
#include "save_gate.h"
#include "s2a_launch_ctx.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdlib.h>
#include <string.h>

extern char *S2A_SaveTransfer(const char *request) __attribute__((weak_import));
extern void S2A_SaveTransferFree(char *response) __attribute__((weak_import));
void s2a_host_shutdown(const char *why);
static NSString *const ManageKey = @"S2ASaveManageOnOpen";
// 「下次打开先进存档管理」分两级：ManageKey 是 App 级（iOS 设置开关、主屏快捷操作冷启动置，下次开哪款都进）；
//   游戏内「退出并管理存档」只记这一款：多款容器按游戏号另起一个键，开别的游戏不弹；单款包一个进程只一款，就是 ManageKey 本身
static NSString *game_manage_key(void) {
    // 启动上下文里有本款模块目录就用它（多款容器选了游戏；或直接按 S2A_LAUNCH_CTX 起的那一款——那时 s2a_launch_ctx_container() 判「不是容器」，
    //   原先就去包根找 save-identity.json，多款容器包根没有，存档页 / iCloud 同步 / 测试开关都取不到本款身份）
    const s2a_launch_ctx *c = s2a_launch_ctx_get();
    return c && c->game_id[0] ? [ManageKey stringByAppendingFormat:@".%s", c->game_id] : ManageKey;
}

// 存档位置 SaveData（services/S2A.Services/SaveFolder.cs，11-09 维护者定）：本款文档根下的 SaveData（多款容器 Documents/games/<游戏号>/SaveData，
//   单款包 Documents/SaveData），里面是原生存档格式；放进去下次开始游戏就读，退出时写回最新进度。这里按同一规则算出「文件」App 里的路径给玩家看
//   （文件夹先建出来，「文件」App 里马上看得到），上次没读进来的原因（数据根 savedata-problem.txt，服务层写）接在后面
//   本地玩家（#623）：存档位置是当前玩家的那个文件夹（第一个玩家 SaveData，同 settings.m s2a_ios_local_player），没读进来的原因也按玩家分文件
//   （services/S2A.Services/SaveFolder.cs ProblemPath）；页上写明当前玩家，以及导出 / 导入 / iCloud 是这款所有玩家的存档一起
// 本款存档位置的绝对路径（当前玩家的那个文件夹）：页上显示的路径、导入其他工具的存档放到哪（#2070，服务层 saveFolder）都按它
static NSString *save_folder(void) {
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSDictionary<NSString *, NSString *> *player = s2a_ios_local_player(NSUserDefaults.standardUserDefaults);
    const char *f = getenv("S2A_SAVE_FOLDER"), *d = getenv("S2A_DOCS");   // 同 ios/settings.m 起局时给服务层的那条；存档页在起局前打开时按同一规则算
    NSString *folder = f && *f ? @(f) : [(d && *d ? @(d) : documents) stringByAppendingPathComponent:player ? player[@"folder"] : @"SaveData"];
    [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    return folder;
}
static NSString *save_location(NSString *root) {
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSDictionary<NSString *, NSString *> *player = s2a_ios_local_player(NSUserDefaults.standardUserDefaults);
    NSString *folder = save_folder();
    char *rd = realpath(documents.fileSystemRepresentation, NULL), *rf = realpath(folder.fileSystemRepresentation, NULL);
    NSString *base = rd ? @(rd) : documents, *full = rf ? @(rf) : folder; free(rd); free(rf);
    if (![full hasPrefix:[base stringByAppendingString:@"/"]]) return @"";
    NSString *path = [[[full substringFromIndex:base.length + 1] pathComponents] componentsJoinedByString:@" → "];
    NSString *name = NSBundle.mainBundle.infoDictionary[@"CFBundleDisplayName"] ?: @"S2Arm";
    NSString *text = [NSString stringWithFormat:S2AL(@"save.location"), name, path, name];
    if (player) text = [[NSString stringWithFormat:S2AL(@"save.players"), player[@"name"].length ? player[@"name"] : S2AL(@"save.player.first")] stringByAppendingFormat:@"\n\n%@", text];
    NSString *problem = player && player[@"uid"].length ? [NSString stringWithFormat:@"savedata-problem-%@.txt", player[@"uid"]] : @"savedata-problem.txt";
    NSString *why = root ? [[NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:problem] encoding:NSUTF8StringEncoding error:nil]
                               stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] : nil;
    return why.length ? [text stringByAppendingFormat:@"\n\n%@", [NSString stringWithFormat:S2AL(@"save.location.problem"), why]] : text;
}

static NSString *data_root(void) {
    const char *configured = getenv("S2A_RYU_DATA");
    NSString *root = configured ? [NSString stringWithUTF8String:configured] :
        [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"S2A/ryujinx"];
    // Foundation may shorten /private/var back to /var. Resolve an existing
    // ancestor with POSIX realpath, including before the first save is created.
    NSString *ancestor = root;
    NSMutableArray<NSString *> *suffix = [NSMutableArray new];
    while (![NSFileManager.defaultManager fileExistsAtPath:ancestor] && ancestor.length > 1) {
        [suffix insertObject:ancestor.lastPathComponent atIndex:0];
        ancestor = ancestor.stringByDeletingLastPathComponent;
    }
    char *resolved = realpath(ancestor.fileSystemRepresentation, NULL);
    if (!resolved) return nil;
    NSString *canonical = [NSFileManager.defaultManager stringWithFileSystemRepresentation:resolved length:strlen(resolved)];
    free(resolved);
    for (NSString *part in suffix) canonical = [canonical stringByAppendingPathComponent:part];
    return canonical;
}
static NSString *work_root(void) {
    return [[data_root() stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"save-transfer"];
}
// 这款的存档身份：多款容器在本款随行目录 games/<游戏号>/save-identity.json（pack/ship_ext.py 放的），单款包在 App 包根。
//   原先一律读包根：容器里读不到，存档管理页报「缺少游戏身份」，容器玩家没有存档入口（审计 P2-170 壳那一半）
static NSDictionary *game_identity(void) {
    // 启动上下文里有本款模块目录、且目录里有 save-identity.json 就用它：多款容器选了游戏，或直接按 S2A_LAUNCH_CTX 起的那一款（那时 s2a_launch_ctx_container()
    //   判「不是容器」，原先就去包根找、多款容器包根没有，存档页 / iCloud 同步 / 测试开关都取不到本款身份）；单款包模块目录里没有它，照旧取包根那份
    const s2a_launch_ctx *c = s2a_launch_ctx_get();
    NSString *mod = c && c->module_dir[0] ? [@(c->module_dir) stringByAppendingPathComponent:@"save-identity.json"] : nil;
    NSURL *url = mod && [NSFileManager.defaultManager fileExistsAtPath:mod] ? [NSURL fileURLWithPath:mod]
                                                                         : [NSBundle.mainBundle URLForResource:@"save-identity" withExtension:@"json"];
    NSData *data = url ? [NSData dataWithContentsOfURL:url] : nil;
    id game = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [game isKindOfClass:NSDictionary.class] ? game : nil;
}
// 服务层错误说明的语言（#316）：请求带上 App 语言，服务层（services/S2A.Services/SaveTransfer.cs T）按它出中文或英文；判据同 S2AL 字符串表
static NSString *save_language(void) { return s2a_l10n_zh() ? @"zh-Hans" : @"en"; }
// iCloud 存档同步（shell/Sources/S2ArmShellHost/CloudSaves.swift 经 s2a_shell.h 调）：本款作业公共字段 / 转发一次作业。强定义盖过 S2ArmShellBridge 的弱缺省
char *s2a_shell_save_context(void) {
    @autoreleasepool {
        NSString *root = data_root(); NSDictionary *game = game_identity();
        if (!root || !game || !S2A_SaveTransfer || !S2A_SaveTransferFree) return NULL;
        NSData *json = [NSJSONSerialization dataWithJSONObject:@{ @"game": game, @"dataRoot": root, @"workRoot": work_root(), @"language": save_language() } options:0 error:nil];
        return json ? strndup(json.bytes, json.length) : NULL;
    }
}
char *s2a_shell_save_request(const char *json) {
    if (!json || !S2A_SaveTransfer || !S2A_SaveTransferFree) return NULL;
    char *raw = S2A_SaveTransfer(json);
    char *copy = raw ? strdup(raw) : NULL;
    if (raw) S2A_SaveTransferFree(raw);
    return copy;
}
// 测试开关 S2A_TEST_SAVE_OP（真机验收「导出存档 → 删除重装 S2Arm → 导回存档」，维护者 11-15 定的 #569 处理要求这条路真能走通；无人点存档页）：
//   export:<路径> = 把本款当前存档导出成 .s2asave；restore:<路径>（相对路径按 App 容器根） = 先 inspect 再按它给的指纹 restore（同界面「检查 → 确认」两步）。
//   起局前（ios/app.m startHost 开头：游戏已选定、宿主还没起）跑一次，走与存档页同一个服务层入口；结果一行进日志。只认环境变量，玩家从图标打开不带
static NSString *test_save_call(NSDictionary *extra) {
    char *ctx = s2a_shell_save_context();
    if (!ctx) return [NSString stringWithFormat:@"no save context: dataRoot=%@ game=%@ S2A_SaveTransfer=%s", data_root() ?: @"nil", game_identity() ? @"ok" : @"nil",
                      S2A_SaveTransfer && S2A_SaveTransferFree ? "linked" : "missing"];
    NSMutableDictionary *req = [[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:ctx length:strlen(ctx)] options:NSJSONReadingMutableContainers error:nil] mutableCopy]; free(ctx);
    [req addEntriesFromDictionary:extra];
    NSData *json = [NSJSONSerialization dataWithJSONObject:req options:0 error:nil]; NSString *js = [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
    char *raw = s2a_shell_save_request(js.UTF8String); NSString *r = raw ? @(raw) : @"(no response)"; free(raw); return r;
}
void s2a_ios_test_save_op(void) {
    const char *e = getenv("S2A_TEST_SAVE_OP"); if (!e || !*e) return;
    @autoreleasepool {
        NSString *op = @(e); NSRange c = [op rangeOfString:@":"]; if (c.location == NSNotFound) { fprintf(stderr, "[S2A 测试存档] 开关格式不对：%s\n", e); return; }
        NSString *kind = [op substringToIndex:c.location], *path = [op substringFromIndex:c.location + 1];
        if (![path hasPrefix:@"/"]) {   // 相对路径按 App 容器根（电脑端经 devicectl 推拉用的就是容器内相对路径）；容器根取 realpath：/var 是指向 /private/var 的链接，服务层拒收含链接的路径
            char *home = realpath(NSHomeDirectory().fileSystemRepresentation, NULL);
            path = [(home ? @(home) : NSHomeDirectory()) stringByAppendingPathComponent:path]; free(home);
        }
        if ([kind isEqualToString:@"export"]) { fprintf(stderr, "[S2A 测试存档] 导出 → %s：%s\n", path.UTF8String, test_save_call(@{@"operation": @"export", @"path": path}).UTF8String); return; }
        if (![kind isEqualToString:@"restore"]) { fprintf(stderr, "[S2A 测试存档] 不认识的操作：%s\n", e); return; }
        NSString *ins = test_save_call(@{@"operation": @"inspect", @"path": path});
        NSDictionary *r = [NSJSONSerialization JSONObjectWithData:[ins dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        NSDictionary *res = [r isKindOfClass:NSDictionary.class] && [r[@"ok"] boolValue] ? r[@"result"] : nil;
        if (!res) { fprintf(stderr, "[S2A 测试存档] 导回 %s：检查没过：%s\n", path.UTF8String, ins.UTF8String); return; }
        fprintf(stderr, "[S2A 测试存档] 导回 %s：%s\n", path.UTF8String,
                test_save_call(@{@"operation": @"restore", @"path": path, @"archiveSha256": res[@"archiveSha256"] ?: @"", @"targetFingerprint": res[@"targetFingerprint"] ?: @""}).UTF8String);
    }
}
// 列不出事务目录时记下原因（审计 P2-9：原先读不到也当「有未完成事务」，恢复再失败时提示说的是事务，玩家无从下手）。
//   仍进存档页、仍先试恢复（服务层能读到就照常恢复）；恢复失败时提示写明是「读不到操作记录目录」及系统原因
static NSString *g_journal_error;
static BOOL recovery_pending(void) {
    g_journal_error = nil;
    if (!data_root()) return YES;
    NSString *directory = [work_root() stringByAppendingPathComponent:@"save-transactions"];
    if (![NSFileManager.defaultManager fileExistsAtPath:directory]) return NO;
    NSError *error = nil;
    NSArray *entries = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:&error];
    // Even an unreadable journal directory must enter management, never skip recovery.
    if (error) { g_journal_error = error.localizedDescription ?: S2AL(@"save.unknownReason"); return YES; }
    for (NSString *entry in entries) if ([entry.pathExtension isEqualToString:@"json"]) return YES;
    return NO;
}
int s2a_ios_save_management_requested(void) {
    @autoreleasepool {
        NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
        return [d boolForKey:ManageKey] || [d boolForKey:game_manage_key()] || recovery_pending();
    }
}

@interface S2ASaveManager : UIViewController <UIDocumentPickerDelegate>
@property(copy) void (^startGame)(void);
@property(strong) NSDictionary *game;
@property(strong) UILabel *status;
@property(strong) UIButton *importButton, *exportButton, *backupButton, *startButton, *overrideButton, *quitButton, *previousButton;
@property BOOL hasPrevious;   // 有上一份存档（<存档>/.s2a_prev，服务层 previous）才显示「换到上一份」
@property(strong) UIActivityIndicatorView *spinner;
@property(strong) NSDictionary *preview;
@property(copy) NSString *importPath, *backupPath, *temporaryExportPath;
@property(weak) UIDocumentPickerViewController *exportPicker;
@property(copy) NSString *dataRoot, *workRoot;
@property(strong) dispatch_queue_t worker;
@property BOOL busy, hasSave, recoveryOverridden;
@property enum s2a_save_phase phase;
@end

@implementation S2ASaveManager
S2A_VERTICAL_BAR_OFF
// 存档页不跟游戏的横 / 竖屏设置走（#1576）：页上有导出命名、文件选择这类要打字的系统窗口，锁横屏时键盘一弹就只剩一条缝；
//   普通 iPhone 横竖随手机转（不含倒置），可自由缩放的窗口（Duo 内屏 / iPad）照旧全部方向。点「开始游戏」后游戏页按自己的设置再转
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return s2a_ios_play_resizable() ? UIInterfaceOrientationMaskAll : UIInterfaceOrientationMaskAllButUpsideDown; }
- (UILabel *)label:(NSString *)text style:(UIFontTextStyle)style {
    UILabel *label = [UILabel new]; label.text = text; label.numberOfLines = 0;
    label.font = [UIFont preferredFontForTextStyle:style]; label.adjustsFontForContentSizeCategory = YES;
    return label;
}
- (UIButton *)button:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:title forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    button.titleLabel.adjustsFontForContentSizeCategory = YES; button.titleLabel.numberOfLines = 0;
    UIButtonConfiguration *configuration = [UIButtonConfiguration plainButtonConfiguration];
    configuration.contentInsets = NSDirectionalEdgeInsetsMake(12, 16, 12, 16);
    button.configuration = configuration;
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.worker = dispatch_queue_create("dev.s2arm.save-transfer", DISPATCH_QUEUE_SERIAL);
    self.phase = s2a_save_next(S2A_SAVE_CHECKING, recovery_pending() ? S2A_SAVE_EV_OPEN_TX : S2A_SAVE_EV_OPEN_NO_TX);
    UIScrollView *scroll = [UIScrollView new]; scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    UIStackView *stack = [UIStackView new]; stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12; stack.translatesAutoresizingMaskIntoConstraints = NO; [scroll addSubview:stack];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor], [scroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:20],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-20],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-48]]];
    UILabel *title = [self label:S2AL(@"save.title") style:UIFontTextStyleTitle1];
    title.accessibilityTraits |= UIAccessibilityTraitHeader; [stack addArrangedSubview:title];
    [stack addArrangedSubview:[self label:S2AL(@"save.intro") style:UIFontTextStyleBody]];
    UILabel *where = [self label:@"" style:UIFontTextStyleFootnote]; where.textColor = UIColor.secondaryLabelColor; where.accessibilityIdentifier = @"save.location";
    self.status = [self label:S2AL(@"save.checking") style:UIFontTextStyleBody]; [stack addArrangedSubview:self.status];
    self.spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    self.spinner.hidesWhenStopped = YES; [stack addArrangedSubview:self.spinner];
    self.exportButton = [self button:S2AL(@"save.export") action:@selector(exportSave)];
    self.importButton = [self button:S2AL(@"save.import") action:@selector(importSave)];
    self.backupButton = [self button:S2AL(@"save.exportBackup") action:@selector(exportBackup)];
    self.startButton = [self button:S2AL(@"save.start") action:@selector(start)];
    // 恢复失败时才出现：单独确认后放开「开始游戏」（审计 P0-1：失败分支不放开，玩家这次安装就再也进不了游戏）
    self.overrideButton = [self button:S2AL(@"save.override") action:@selector(confirmOverride)];
    // #1206「退出」：清掉「下次先进存档管理」直接退出，下次打开照常进游戏（多款容器进游戏库）；有待恢复事务时不出
    self.quitButton = [self button:S2AL(@"save.leave") action:@selector(quitManagement)];
    self.previousButton = [self button:S2AL(@"save.previous.button") action:@selector(switchPrevious)];   // 回到上一份存档：可逆，再点一次换回来
    self.previousButton.accessibilityHint = S2AL(@"save.previous.detail"); self.previousButton.hidden = YES;
    for (UIButton *button in @[self.exportButton, self.importButton, self.backupButton, self.previousButton, self.startButton, self.overrideButton, self.quitButton]) [stack addArrangedSubview:button];
    [stack addArrangedSubview:where];
    self.dataRoot = data_root();
    where.text = save_location(self.dataRoot);
    if (!self.dataRoot) {
        self.phase = s2a_save_next(self.phase, S2A_SAVE_EV_NO_ROOT);
        self.status.text = S2AL(@"save.noRoot");
        [self updateButtons]; return;
    }
    self.workRoot = [[self.dataRoot stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"save-transfer"];
    NSDictionary *game = game_identity();
    if (!game || !S2A_SaveTransfer || !S2A_SaveTransferFree) {
        if (self.phase == S2A_SAVE_CHECKING) { [self blockRecovery:S2A_SAVE_EV_NO_SERVICE reason:S2AL(@"save.noService.recover")]; return; }
        self.status.text = S2AL(@"save.noService");
        [self updateButtons]; return;
    }
    self.game = game;
    [self request:@{ @"operation": @"recover" } completion:^(NSDictionary *result) {
        self.phase = s2a_save_next(self.phase, S2A_SAVE_EV_RECOVER_OK); [self refresh];
    }];
}
- (void)updateButtons {
    struct s2a_save_buttons b = s2a_save_buttons(self.phase, self.recoveryOverridden, self.game != nil, self.hasSave, self.busy);
    self.importButton.enabled = b.import; self.exportButton.enabled = b.export_;
    self.backupButton.enabled = !self.busy && self.backupPath.length;
    self.previousButton.hidden = !self.hasPrevious; self.previousButton.enabled = !self.busy && b.import;   // 能导入（没在恢复失败的状态里）才能换
    self.startButton.enabled = b.start; self.overrideButton.hidden = !b.override; self.quitButton.hidden = !b.quit;
    if (self.busy) [self.spinner startAnimating]; else [self.spinner stopAnimating];
}
// 恢复失败：说清楚数据在哪、怎么拷走，并给「仍要开始游戏」（单独确认）。事务记录原样保留，下次打开照常先试恢复（新版本可能恢复得了）
- (void)blockRecovery:(enum s2a_save_event)event reason:(NSString *)reason {
    self.phase = s2a_save_next(self.phase, event); self.recoveryOverridden = NO; self.preview = nil;
    [self showError:[NSString stringWithFormat:S2AL(@"save.blocked"), reason, NSBundle.mainBundle.infoDictionary[@"CFBundleDisplayName"] ?: @"S2Arm"]];
}
- (void)confirmOverride {
    // 读不到存档位置（审计 18:00 P2-14）与恢复失败共用这个确认，但不冒充「有没做完的事务」：各说各的
    BOOL noRoot = self.phase == S2A_SAVE_NO_ROOT;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:S2AL(@"save.override.title") message:noRoot ? S2AL(@"save.override.noRoot") : S2AL(@"save.override.message") preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:S2AL(@"common.cancel") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:noRoot ? S2AL(@"save.start") : S2AL(@"save.override.confirm") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        (void)action; self.recoveryOverridden = YES;
        self.status.text = noRoot ? S2AL(@"save.override.doneNoRoot") : S2AL(@"save.override.done"); [self updateButtons];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
// 读屏播报分成功 / 出错（审计 P2-12：原先成功也走 showError，读屏把「备份已导出」听成和失败一样的播报）：出错的播报前加「出错：」
- (void)showError:(NSString *)message {
    self.status.text = message; [self updateButtons];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, [S2AL(@"save.errorPrefix") stringByAppendingString:message]);
}
- (void)showStatus:(NSString *)message {
    self.status.text = message; [self updateButtons];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, message);
}
// 成功 / 取消的结果：临时副本清理失败（warning）才算出错
- (void)showResult:(NSString *)message warning:(NSString *)warning {
    if (warning) [self showError:[NSString stringWithFormat:@"%@\n%@", message, warning]];
    else [self showStatus:message];
}
- (NSString *)removeTemporaryFile:(NSString *)path {
    if (!path.length) return nil;
    NSError *error = nil;
    if ([NSFileManager.defaultManager removeItemAtPath:path error:&error] ||
        ([error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSFileNoSuchFileError)) return nil;
    return [NSString stringWithFormat:S2AL(@"save.tempCleanupFailed"), path.lastPathComponent];
}
- (NSString *)clearImport {
    NSString *warning = [self removeTemporaryFile:self.importPath];
    if (!warning) self.importPath = nil;
    return warning;
}
- (NSString *)clearExport {
    NSString *warning = [self removeTemporaryFile:self.temporaryExportPath];
    if (!warning) self.temporaryExportPath = nil;
    return warning;
}
- (void)request:(NSDictionary *)operation completion:(void (^)(NSDictionary *))completion {
    // 上一项还没做完：说一声再返回（审计 P2-23：原先静默丢掉，玩家只看到原来那句「正在处理」）。按钮忙时本就禁用，走到这里的是界面外的入口
    if (self.busy) { [self showStatus:S2AL(@"save.busy")]; return; }
    self.busy = YES; self.status.text = S2AL(@"save.processing"); [self updateButtons];
    NSMutableDictionary *request = [operation mutableCopy];
    request[@"game"] = self.game; request[@"dataRoot"] = self.dataRoot; request[@"workRoot"] = self.workRoot; request[@"language"] = save_language();
    request[@"saveFolder"] = save_folder();   // #2070：选到其他工具的存档时，服务层认出后放进这里，下次起局读入
    dispatch_async(self.worker, ^{
        @autoreleasepool {
            NSError *error = nil;
            NSData *json = [NSJSONSerialization dataWithJSONObject:request options:0 error:&error];
            NSDictionary *response = nil;
            if (json) {
                NSString *text = [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
                char *raw = S2A_SaveTransfer(text.UTF8String);
                if (raw) {
                    NSData *bytes = [[NSString stringWithUTF8String:raw] dataUsingEncoding:NSUTF8StringEncoding];
                    S2A_SaveTransferFree(raw);
                    if (bytes) response = [NSJSONSerialization JSONObjectWithData:bytes options:0 error:&error];
                }
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                self.busy = NO;
                if (![response isKindOfClass:NSDictionary.class] || ![response[@"ok"] boolValue]) {
                    NSString *message = [response[@"error"] isKindOfClass:NSDictionary.class] ? response[@"error"][@"message"] : nil;
                    NSString *cleanup = nil;
                    if ([operation[@"operation"] isEqual:@"inspect"] || [operation[@"operation"] isEqual:@"restore"]) cleanup = [self clearImport];
                    message = message ?: S2AL(@"save.serviceInvalid");
                    if (cleanup) message = [NSString stringWithFormat:@"%@\n%@", message, cleanup];
                    // 恢复失败、或还原失败后留下了没做完的事务：不能只把按钮按灰就返回。还原失败但没留事务（已回滚）照常可开始
                    if ([operation[@"operation"] isEqual:@"recover"])
                        [self blockRecovery:S2A_SAVE_EV_RECOVER_FAIL reason:g_journal_error ? [NSString stringWithFormat:S2AL(@"save.journalUnreadable"), g_journal_error, message] : message];
                    else if ([operation[@"operation"] isEqual:@"restore"] && recovery_pending()) [self blockRecovery:S2A_SAVE_EV_RESTORE_FAIL_TX reason:message];
                    else [self showError:message];
                    return;
                }
                completion(response[@"result"]); [self updateButtons];
            });
        }
    });
}
- (void)refresh {
    [self request:@{ @"operation": @"status" } completion:^(NSDictionary *result) {
        self.hasSave = [result[@"hasSave"] boolValue];
        self.backupPath = [result[@"previousBackup"] isKindOfClass:NSString.class] ? result[@"previousBackup"] : nil;
        self.status.text = self.hasSave ? S2AL(@"save.status.hasSave") : S2AL(@"save.status.noSave");
        [self request:@{ @"operation": @"previous" } completion:^(NSDictionary *prev) { self.hasPrevious = [prev[@"hasPrevious"] boolValue]; }];
    }];
}
// 回到上一份存档：已提交版本与上一份互换（服务层 restorePrevious，可逆），换完告诉玩家可以随时换回来
- (void)switchPrevious {
    [self request:@{ @"operation": @"restorePrevious" } completion:^(__unused NSDictionary *result) { self.status.text = S2AL(@"save.previous.done"); }];
}
- (void)start {
    if (!s2a_save_buttons(self.phase, self.recoveryOverridden, self.game != nil, self.hasSave, self.busy).start) return;
    // 玩家确认过「仍要开始」就不再先跑恢复（它刚失败过，失败分支不会走到 finishManagement）
    if (self.game && !self.recoveryOverridden) {
        [self request:@{ @"operation": @"recover" } completion:^(NSDictionary *result) {
            (void)result; [self finishManagement];
        }];
    } else [self finishManagement];
}
- (void)clearManageFlags {
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:ManageKey];
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:game_manage_key()];
    [NSUserDefaults.standardUserDefaults synchronize];
}
- (void)finishManagement {
    [self clearManageFlags];
    self.startGame();
}
// 宿主还没起（这页在起宿主之前）：没有游戏状态要收，不走 s2a_host_shutdown 的收局流程，复位开关后直接结束进程
- (void)quitManagement {
    if (!s2a_save_buttons(self.phase, self.recoveryOverridden, self.game != nil, self.hasSave, self.busy).quit) return;
    [self clearManageFlags];
    exit(0);
}
- (void)presentExport:(NSString *)path {
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[[NSURL fileURLWithPath:path]] asCopy:YES];
    picker.directoryURL = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    picker.delegate = self; self.exportPicker = picker;
    s2a_present_picker(self, picker, "save-export");
}
- (void)exportSave {
    // 11-13 维护者「不许堵门」：上次的临时副本删不掉不挡这次（新副本用新名字，互不冲突）；只记一行日志，下次再清
    NSString *warning = [self clearExport];
    if (warning) fprintf(stderr, "[S2A 存档] 上次的临时副本没删掉，照常继续：%s\n", warning.UTF8String);
    NSString *name = [NSString stringWithFormat:@"%@-%@.s2asave", self.game[@"id"], NSUUID.UUID.UUIDString];
    NSString *path = [[[self.dataRoot stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"save-exports"] stringByAppendingPathComponent:name];
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:[path stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:&error]) { [self showError:S2AL(@"save.exportDirFailed")]; return; }
    [self request:@{ @"operation": @"export", @"path": path } completion:^(NSDictionary *result) {
        self.temporaryExportPath = result[@"outputPath"];
        self.status.text = S2AL(@"save.exportReady"); [self presentExport:result[@"outputPath"]];
    }];
}
- (void)exportBackup { if (self.backupPath.length) [self presentExport:self.backupPath]; }
- (void)importSave {
    // 11-13 维护者「不许堵门」：上次的临时副本删不掉不挡这次（新副本用新名字，互不冲突）；只记一行日志，下次再清
    NSString *warning = [self clearImport];
    if (warning) fprintf(stderr, "[S2A 存档] 上次的临时副本没删掉，照常继续：%s\n", warning.UTF8String);
    // #2070：其他工具的存档常是一个文件夹，文件夹也能选（服务层按存档位置的同一套规则认）
    // asCopy 必须是 NO：YES 时系统不让选文件夹（右上「打开」一直是灰的，0.6.9 iPad 报修「打不开存档文件夹」），
    //   NO 给的是带授权的原地址，下面 didPick 里 startAccessing 后自己拷进 imports/ 临时副本，选文件也照旧
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData, UTTypeFolder] asCopy:NO];
    picker.directoryURL = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    picker.allowsMultipleSelection = NO; picker.delegate = self;
    s2a_present_picker(self, picker, "save-import");
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (controller == self.exportPicker) {
        self.exportPicker = nil;
        // A destination chosen inside our export folder becomes the user's file.
        if (self.temporaryExportPath.length &&
            [urls.firstObject.URLByResolvingSymlinksInPath isEqual:[NSURL fileURLWithPath:self.temporaryExportPath].URLByResolvingSymlinksInPath]) self.temporaryExportPath = nil;
        NSString *warning = [self clearExport];
        [self showResult:S2AL(@"save.exported") warning:warning];
        return;
    }
    NSURL *url = urls.firstObject; if (!url || self.busy) return;
    self.busy = YES; self.status.text = S2AL(@"save.reading"); [self updateButtons];
    dispatch_async(self.worker, ^{
        BOOL scoped = [url startAccessingSecurityScopedResource];
        NSString *folder = [self.workRoot stringByAppendingPathComponent:@"imports"];
        // 临时副本保留原扩展名（文件夹没有）：是不是我方备份由服务层按内容认，不靠名字
        NSString *path = [folder stringByAppendingPathComponent:url.pathExtension.length ? [NSUUID.UUID.UUIDString stringByAppendingPathExtension:url.pathExtension] : NSUUID.UUID.UUIDString];
        NSError *error = nil;
        BOOL copied = [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:&error] &&
            [NSFileManager.defaultManager copyItemAtURL:url toURL:[NSURL fileURLWithPath:path] error:&error];
        if (scoped) [url stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            self.busy = NO;
            self.importPath = path;
            if (!copied) {
                NSString *warning = [self clearImport];
                [self showError:warning ? [NSString stringWithFormat:S2AL(@"save.readFailed.detail"), warning] : S2AL(@"save.readFailed")];
                return;
            }
            [self request:@{ @"operation": @"inspect", @"path": path } completion:^(NSDictionary *result) {
                self.preview = result; [self confirmRestore];
            }];
        });
    });
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    if (controller != self.exportPicker) { self.status.text = S2AL(@"save.pickCancelled"); return; }
    self.exportPicker = nil;
    NSString *warning = [self clearExport];
    [self showResult:S2AL(@"save.exportCancelled") warning:warning];
}
- (void)confirmRestore {
    BOOL community = [self.preview[@"community"] boolValue];   // #2070：其他模拟器 / 工具的存档，没有我方备份的游戏名 / 版本 / 用户清单
    NSDictionary *manifest = self.preview[@"manifest"];
    NSString *users = [manifest[@"users"][@"ids"] componentsJoinedByString:@"\n"];
    NSString *size = [NSByteCountFormatter stringFromByteCount:[self.preview[@"totalBytes"] longLongValue] countStyle:NSByteCountFormatterCountStyleFile];
    NSString *message = community ? [NSString stringWithFormat:S2AL(@"save.community.message"), self.preview[@"fileCount"], size] : [NSString stringWithFormat:S2AL(@"save.restore.message"), manifest[@"game"][@"name"], manifest[@"game"][@"version"], manifest[@"createdAt"], users.length ? users : S2AL(@"save.deviceSave"), self.preview[@"fileCount"], size];
    // 版本 / DLC 组合、账号归属对不上：服务层不再拒绝，给出提示句（已按界面语言），确认前放在说明最前面
    NSArray *warnings = [self.preview[@"warnings"] isKindOfClass:NSArray.class] ? self.preview[@"warnings"] : @[];
    if (warnings.count) message = [NSString stringWithFormat:@"⚠️ %@\n\n%@", [warnings componentsJoinedByString:@"\n⚠️ "], message];
    self.status.text = S2AL(@"save.inspected");
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:S2AL(@"save.restore.title") message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:S2AL(@"common.cancel") style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
        (void)action; self.preview = nil;
        NSString *warning = [self clearImport];
        [self showResult:S2AL(@"save.restoreCancelled") warning:warning];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:S2AL(@"save.restore.confirm") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        (void)action;
        [self request:@{ @"operation": @"restore", @"path": self.importPath, @"archiveSha256": self.preview[@"archiveSha256"], @"targetFingerprint": self.preview[@"targetFingerprint"] } completion:^(NSDictionary *result) {
            self.preview = nil; self.phase = s2a_save_next(self.phase, S2A_SAVE_EV_RESTORE_OK);
            self.backupPath = [result[@"previousBackup"] isKindOfClass:NSString.class] ? result[@"previousBackup"] : nil;
            self.hasSave = YES;
            NSString *warning = [self clearImport];
            NSString *message = [result[@"pending"] boolValue] ? S2AL(@"save.community.done") : S2AL(@"save.restored");   // #2070：放进了存档位置，下次起局才读入
            [self showResult:message warning:warning];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
@end

UIViewController *s2a_ios_save_manager(void (^start_game)(void)) {
    S2ASaveManager *manager = [S2ASaveManager new]; manager.startGame = start_game; return manager;
}
// 游戏内「存档管理」确认框：「退出游戏」只收尾退出；「退出并管理存档」才记下次先进存档管理（只记这一款）。
//   原先只有一个「已保存，退出游戏」且必置开关：v0.2.2 起游戏里只有它写着「退出游戏」，玩家拿它当退出，之后每次打开都先进存档页
void s2a_ios_save_present(UIViewController *presenter) {
    if (!presenter) return;
    presenter = s2a_top_presenter(presenter);
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:S2AL(@"save.quit.title") message:S2AL(@"save.quit.message") preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:S2AL(@"save.quit.back") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:S2AL(@"save.quit.only") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *item) {
        (void)item;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ s2a_host_shutdown("退出游戏"); });
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:S2AL(@"save.quit.manage") style:UIAlertActionStyleDefault handler:^(UIAlertAction *item) {
        (void)item; [NSUserDefaults.standardUserDefaults setBool:YES forKey:game_manage_key()];
        [NSUserDefaults.standardUserDefaults synchronize];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ s2a_host_shutdown("打开存档管理"); });
    }]];
    [presenter presentViewController:alert animated:YES completion:nil];
}
// ---------- 回到上一份存档：开始前问（不拦、不自动覆盖） ----------
// 只在两种情况问（同 Mac pack/jobs/save_previous.py 口径；只是闪退过一次不问，免得玩家误点白丢最近的进度）：
//   开局就崩：上一局死在前台、死的就是这一款（会话标记 runtime/src/sesdiag.c，同 app.m crash_auto_off），且那局活了不到 EARLY_SECONDS
//     ——那局 host.log 的创建时刻（起局时轮换新建，runtime/src/os.c log_archive 同样按它认会话开始）到最后一次写入；
//   存档护栏拦下过：服务层 previous 的 guardRed（<存档>/.s2a_guard）。
//   且这款有上一份存档：
//   问「这款上次没能正常读存档，要回到上一份存档吗？」——「回到上一份」走 restorePrevious（换完再告诉玩家可以随时换回来）/「照常开始」。
//   要问就返回 1，玩家选完调 go；不用问返回 0，调用方自己接着起局。起宿主之前调（存档还没挂载）
#include "s2a_sesdiag.h"
#include "s2a_docs.h"
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
static const double EARLY_SECONDS = 60;   // 同 pack/jobs/save_previous.py EARLY_SECONDS
static NSDictionary *previous_call(NSString *op) {
    char *ctx = s2a_shell_save_context();
    if (!ctx) return nil;
    NSMutableDictionary *req = [[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:ctx length:strlen(ctx)] options:NSJSONReadingMutableContainers error:nil] mutableCopy]; free(ctx);
    if (![req isKindOfClass:NSMutableDictionary.class]) return nil;
    req[@"operation"] = op;
    NSData *json = [NSJSONSerialization dataWithJSONObject:req options:0 error:nil];
    char *raw = json ? s2a_shell_save_request([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String) : NULL;
    NSDictionary *r = raw ? [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:raw length:strlen(raw)] options:0 error:nil] : nil; free(raw);
    return [r isKindOfClass:NSDictionary.class] ? r : nil;
}
NSString *g_s2a_prev_last_game;   // 多款容器点开始前的上一款（app.m library_play_go 覆盖 S2ALastGameId 前记下；此时那个键已是这一款）；单款 App 一直 nil
static BOOL previous_started_host(NSString *gid);
static BOOL previous_died_here(void) {
    char d[1024]; if (s2a_container_docs(d, sizeof d) <= 0 || s2a_sesmark_prev(d) != S2A_SESMARK_FG) return NO;
    NSString *last = g_s2a_prev_last_game ?: [NSUserDefaults.standardUserDefaults stringForKey:@"S2ALastGameId"];
    NSString *gid = game_identity()[@"id"];
    if (last.length && gid.length && [last caseInsensitiveCompare:gid] != NSOrderedSame) return NO;   // 单款 App 没记过 = 就是这一款
    return previous_started_host(gid);
}
static double ts_of(struct timespec t) { return (double)t.tv_sec + t.tv_nsec / 1e9; }
static double process_start(void) {
    struct kinfo_proc kp; size_t n = sizeof kp; int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    if (sysctl(mib, 4, &kp, &n, NULL, 0) || !n) return 0;
    return (double)kp.kp_proc.p_starttime.tv_sec + kp.kp_proc.p_starttime.tv_usec / 1e6;
}
// 上一局的 host.log：这个进程起来之前建的 Documents/host.log（预热进程还没轮换），否则 logs/ 里最新的 host-*.log（普通启动构造函数里已轮换）
static NSString *previous_host_log(void) {
    char d[1024]; if (s2a_container_docs(d, sizeof d) <= 0) return nil;
    NSString *docs = @(d), *dir = [docs stringByAppendingPathComponent:@"logs"]; struct stat st;
    NSString *host = [docs stringByAppendingPathComponent:@"host.log"];
    if (!stat(host.fileSystemRepresentation, &st) && ts_of(st.st_birthtimespec) < process_start()) return host;
    NSArray *names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]
                      filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH 'host-' AND SELF ENDSWITH '.log'"]];
    NSString *last = [[names sortedArrayUsingSelector:@selector(compare:)] lastObject];   // host-年月日-时分秒[_序号].log：字典序即时间序
    return last ? [dir stringByAppendingPathComponent:last] : nil;
}
// 上一局活了多少秒：上一局 host.log 创建时刻到最后写入。取不到回 -1（按「不算开局就崩」）
double s2a_ios_previous_session_seconds(void) {
    NSString *pick = previous_host_log(); struct stat st;
    if (!pick || stat(pick.fileSystemRepresentation, &st)) return -1;
    return ts_of(st.st_mtimespec) - ts_of(st.st_birthtimespec);
}
// 上一局真的为这一款起过宿主：host.log 开头有这一款的「S2A 应用登记：程序号 <号>」（起宿主才打，登记行在前几百行）。
//   进程起来就写 fg 标记（sesdiag.c），停在游戏库没点开始、在前台被关掉的那次也是 fg，没有这一行就不算「这一款开局就崩」
static BOOL previous_started_host(NSString *gid) {
    NSString *pick = previous_host_log();
    NSFileHandle *f = pick ? [NSFileHandle fileHandleForReadingAtPath:pick] : nil;
    NSData *head = [f readDataOfLength:512 * 1024]; [f closeFile];
    NSString *text = head.length ? [[NSString alloc] initWithData:head encoding:NSUTF8StringEncoding] ?: [[NSString alloc] initWithData:head encoding:NSISOLatin1StringEncoding] : nil;
    if (!text.length) return NO;
    NSString *needle = gid.length ? [@"S2A 应用登记：程序号 " stringByAppendingString:gid] : @"S2A 应用登记：程序号 ";
    return [text rangeOfString:needle options:NSCaseInsensitiveSearch].location != NSNotFound;
}
int s2a_ios_previous_save_prompt(UIViewController *presenter, void (^go)(void)) {
    if (!presenter || !go) return 0;
    NSDictionary *p = previous_call(@"previous");
    if (![p[@"ok"] boolValue] || ![p[@"result"][@"hasPrevious"] boolValue]) return 0;
    BOOL guard = [p[@"result"][@"guardRed"] boolValue];
    double lived = previous_died_here() ? s2a_ios_previous_session_seconds() : -1;
    BOOL early = lived >= 0 && lived < EARLY_SECONDS;
    if (!guard && !early) return 0;
    fprintf(stderr, "[S2A 存档] 有上一份存档且%s，开始前问要不要回到上一份\n", guard ? "存档护栏拦下过" : [NSString stringWithFormat:@"上一局开局 %.0f 秒就死在前台", lived].UTF8String);
    UIAlertController *a = [UIAlertController alertControllerWithTitle:S2AL(@"save.previous.ask.title") message:guard ? S2AL(@"save.previous.ask.guard") : S2AL(@"save.previous.ask.crash") preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:S2AL(@"save.previous.ask.start") style:UIAlertActionStyleCancel handler:^(__unused UIAlertAction *x) { go(); }]];
    [a addAction:[UIAlertAction actionWithTitle:S2AL(@"save.previous.ask.restore") style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *x) {
        NSDictionary *r = previous_call(@"restorePrevious");
        BOOL ok = [r[@"ok"] boolValue];
        NSString *why = [r[@"error"] isKindOfClass:NSDictionary.class] ? r[@"error"][@"message"] : nil;
        fprintf(stderr, "[S2A 存档] 回到上一份%s%s\n", ok ? "成功" : "失败：", ok ? "" : (why ?: @"服务层没回应").UTF8String);
        UIAlertController *done = [UIAlertController alertControllerWithTitle:ok ? S2AL(@"save.previous.done.title") : S2AL(@"save.errorPrefix")
                                                                      message:ok ? S2AL(@"save.previous.done") : (why ?: S2AL(@"save.serviceInvalid")) preferredStyle:UIAlertControllerStyleAlert];
        [done addAction:[UIAlertAction actionWithTitle:S2AL(@"save.previous.done.ok") style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *y) { go(); }]];
        [presenter presentViewController:done animated:YES completion:nil];
    }]];
    [presenter presentViewController:a animated:YES completion:nil];
    return 1;
}
