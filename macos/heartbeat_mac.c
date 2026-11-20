// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Mac 心跳（#1050）：只编进 Mac 宿主（launchers/macos/build.mk HOST_SRCS），由 front_mac.m 的 10 秒定时器调。
//   不放 runtime/src/energy.c：它按名配单测 t_energy.c 只链 energy.c，心跳要的上屏帧数（ap_host.c）、客体内存文件后备（vawin.c）那里没有
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include "../runtime/src/native.h"

int s2a_disk_io(uint64_t *rd, uint64_t *wr, uint64_t *pageins);   // runtime/src/energy.c
// Mac 心跳（iOS 的在 ios/app.m，节拍相同）：每 10 秒一行，前端定时器调。#1050：Mac 原先没有周期行，长局后段丢帧、慢动作、实占只涨不降都只能靠玩家自己量。
//   出帧 = 近 10 秒上屏帧数按实际间隔折成每秒（锁 30 掉到二十几就是游戏时间变慢）；读盘 / 缺页读入随丢帧一起涨 = 在等后备文件换页
uint64_t s2a_ap_frames(void);
void s2a_heartbeat_mac(void) {
    static uint64_t lf, lr, lw, lp, lt; static int have; static double tb; if (!tb) { mach_timebase_info_data_t i; mach_timebase_info(&i); tb = (double)i.numer / i.denom; }
    uint64_t f = s2a_ap_frames(), t = mach_absolute_time(), rd = 0, wr = 0, pi = 0; int io = s2a_disk_io(&rd, &wr, &pi) == 0;
    task_vm_info_data_t v; mach_msg_type_number_t c = TASK_VM_INFO_COUNT; unsigned long long fp = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &c) == KERN_SUCCESS ? v.phys_footprint >> 20 : 0;
    double sec = have ? (double)(t - lt) * tb / 1e9 : 0;
    char iob[200] = "读写盘取不到";
    if (io) snprintf(iob, sizeof iob, "读盘 %llu MB、写盘 %llu MB、缺页读入 %llu 次", (unsigned long long)(have && rd >= lr ? (rd - lr) >> 20 : 0),
                     (unsigned long long)(have && wr >= lw ? (wr - lw) >> 20 : 0), (unsigned long long)(have && pi >= lp ? pi - lp : 0));
    LOG("[心跳] 帧 %llu，进程实占 %llu MB，近 %.0f 秒出帧 %llu（%.1f 帧/秒）、%s（客体内存文件后备 %s）", (unsigned long long)f, fp, sec,
        (unsigned long long)(have && f >= lf ? f - lf : 0), sec > 0 && f >= lf ? (double)(f - lf) / sec : 0.0, iob, s2a_guest_fd() >= 0 ? "开" : "关");
    lf = f; lt = t; if (io) { lr = rd; lw = wr; lp = pi; } have = 1;
    { void s2a_guest_backing_log(void); s2a_guest_backing_log(); }   // vawin.c：文件后备开着时报匿名块 / 文件块各多少（溢出模式，没开不打）
    s2a_energy_log();
}
