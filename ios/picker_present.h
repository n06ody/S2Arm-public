// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 选文件 / 存文件界面（UIDocumentPickerViewController）统一弹出并记日志（入口名用英文短名：中文字面量在 ios/ 里只许进日志格式串，tests/ios_l10n_test.py ⑥）（#1939：iPad 上导入 mod / 金手指 / 证书时界面黑屏、卡住，转一下屏幕才出来，
//   诊断里一行相关的都没有）。记三件：弹出时（入口、呈现方及其视图尺寸、是否在窗口里、弹出样式、界面方向）；弹完回调（耗时、界面视图尺寸、是否在窗口里）；
//   之后每秒看一次，界面不在了记「结束」并带停留时长，视图尺寸为 0 或不在窗口里记黄（黑屏多半是这样），最多看 120 秒
#pragma once
#import <UIKit/UIKit.h>
#include "native.h"

static inline void s2a_picker_watch(__weak UIViewController *wp, const char *who, CFAbsoluteTime t0, int n, int odd) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        UIViewController *p = wp;
        if (!p || !p.presentingViewController) { LOG("选文件界面：结束（%s），停留 %.1f 秒", who, CFAbsoluteTimeGetCurrent() - t0); return; }
        CGSize s = p.view.bounds.size; int bad = s.width < 1 || s.height < 1 || !p.view.window;
        if (bad && !odd) LOG("【黄】选文件界面：第 %d 秒（%s）视图 %.0fx%.0f、在窗口里 %d——界面在呈现但看不到（黑屏多半是这样）", n, who, s.width, s.height, p.view.window != nil);
        if (n >= 120) { LOG("选文件界面：%s 已开 120 秒，不再看", who); return; }
        s2a_picker_watch(wp, who, t0, n + 1, bad);
    });
}
// 起始目录记大小、文件数（11-23 #1842：iPad 上选择界面整片空白、左栏连「位置」都不出，11-11 同类是起始目录太大、系统先枚举它）。
//   后台数，最多数 3 秒（超了标「截断」，实际更多）；没给起始目录记一句（系统落在「最近项目」，要汇总整机开了文件共享的 App）
static inline void s2a_picker_dir_stats(NSURL *dir, const char *who) {
    if (!dir) { LOG("选文件界面：起始目录（%s）没指定，系统落在「最近项目」", who); return; }
    const char *name = strdup(who);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent(); long long bytes = 0; long files = 0, top = 0; int cut = 0;
        top = (long)[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir.path error:nil].count;
        NSDirectoryEnumerator *e = [NSFileManager.defaultManager enumeratorAtURL:dir includingPropertiesForKeys:@[NSURLIsRegularFileKey, NSURLTotalFileAllocatedSizeKey] options:0 errorHandler:nil];
        for (NSURL *u in e) {
            if (CFAbsoluteTimeGetCurrent() - t0 > 3) { cut = 1; break; }
            NSNumber *reg = nil, *size = nil;
            [u getResourceValue:&reg forKey:NSURLIsRegularFileKey error:nil];
            if (!reg.boolValue) continue;
            files++; [u getResourceValue:&size forKey:NSURLTotalFileAllocatedSizeKey error:nil]; bytes += size.longLongValue;
        }
        LOG("选文件界面：起始目录（%s）%s：顶层 %ld 项，共 %ld 个文件、%.0f MB（数了 %.0f ms%s）", name, dir.path.UTF8String, top, files,
            bytes / 1048576.0, (CFAbsoluteTimeGetCurrent() - t0) * 1000, cut ? "，超 3 秒截断、实际更多" : "");
        free((void *)name);
    });
}
static inline void s2a_present_picker(UIViewController *host, UIViewController *p, const char *who) {
    CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
    UIView *hv = host.view; CGSize hs = hv.bounds.size;
    long io = 0;
#if !TARGET_OS_TV
    io = (long)hv.window.windowScene.interfaceOrientation;   // tvOS 没有界面方向
#endif
    LOG("选文件界面：弹出（%s），呈现方 %s 视图 %.0fx%.0f、在窗口里 %d、已在呈现别的 %d，样式 %ld，界面方向 %ld，设备 %s",
        who, host ? NSStringFromClass(host.class).UTF8String : "无", hs.width, hs.height, hv.window != nil, host.presentedViewController != nil,
        (long)p.modalPresentationStyle, io, UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? "iPad" : "iPhone");
    if (!host) { LOG("【红】选文件界面：没有可呈现的界面（%s），没弹", who); return; }
#if !TARGET_OS_TV   // tvOS 没有 UIDocumentPickerViewController
    if ([p isKindOfClass:UIDocumentPickerViewController.class]) s2a_picker_dir_stats(((UIDocumentPickerViewController *)p).directoryURL, who);
#endif
    __weak UIViewController *wp = p;
    [host presentViewController:p animated:YES completion:^{
        UIViewController *q = wp; CGSize s = q.view.bounds.size;
        LOG("选文件界面：弹完（%s）%.0f ms，视图 %.0fx%.0f、在窗口里 %d", who, (CFAbsoluteTimeGetCurrent() - t0) * 1000, s.width, s.height, q.view.window != nil);
        s2a_picker_watch(wp, who, t0, 1, 0);
    }];
}
