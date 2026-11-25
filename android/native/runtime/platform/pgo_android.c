// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// 本编译单元不插桩。用 NDK 随附的声明访问本 DSO 已链接的 profile runtime。
#define __LLVM_INSTR_PROFILE_GENERATE 1
#include <profile/instr_prof_interface.h>
#undef __LLVM_INSTR_PROFILE_GENERATE
// NDK 29 的 compiler-rt profile/InstrProfiling.h，归档中实有这两个入口。
int __llvm_profile_set_file_object(FILE *file, int enable_merge);
int __llvm_profile_write_file(void);
const char *s2a_android_gpu_driver_files_dir(void);

static int write_profile(const char *files_dir) {
    if (!files_dir || files_dir[0] != '/') return EINVAL;
    char directory[PATH_MAX], final[PATH_MAX];
    // LLVM 保留 set_filename 传入的指针，缓冲区须活到进程退出。
    static char staging[PATH_MAX];
    if (snprintf(directory, sizeof directory, "%s/pgo", files_dir) >= (int)sizeof directory ||
        snprintf(staging, sizeof staging - sizeof ".profraw" + 1, "%s/s2a-%ld-XXXXXX", directory, (long)getpid()) >=
            (int)(sizeof staging - sizeof ".profraw" + 1)) return ENAMETOOLONG;
    if (mkdir(directory, 0700) && errno != EEXIST) return errno;

    const int fd = mkstemp(staging);
    if (fd < 0) return errno;
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        const int error = errno;
        close(fd);
        unlink(staging);
        return error;
    }

    // 明确覆盖默认路径和外部环境的合并模式。保留流以检查 LLVM 内部 flush 后的错误。
    __llvm_profile_set_filename(staging);
    int error = EIO;
    if (!__llvm_profile_set_file_object(file, 0)) {
        if (!__llvm_profile_write_file() && !fflush(file) && !ferror(file)) {
            struct stat st;
            if (fstat(fd, &st)) error = errno;
            else if (st.st_size > 0) error = 0;
        }
        __llvm_profile_set_file_object(NULL, 0);
    }
    if (fclose(file) && !error) error = errno;
    snprintf(final, sizeof final, "%s.profraw", staging);
    if (!error && rename(staging, final)) error = errno;
    if (error) unlink(staging);
    else fprintf(stderr, "[s2a] PGO：已写入 files/pgo/%s\n", final + strlen(directory) + 1);
    return error;
}

void s2a_android_pgo_write(void) {
    // 入口由一次性的正常收局调用。失败的采集不改变正常退出码，也不发布半份 raw。
    const int error = write_profile(s2a_android_gpu_driver_files_dir());
    if (error) fprintf(stderr, "[s2a] 【红】PGO：未写入剖析数据 errno=%d\n", error);
}
