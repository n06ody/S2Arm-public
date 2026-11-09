// 安卓侧共用文档根绑定（runtime/include/s2a_docs.h 的 S2A_DOCS 契约）：iOS / Mac 由
//   启动上下文（launchctx.c 冻结）把 S2A_DOCS 写进进程环境；安卓没有这个环节，
//   起局时由 runtime 入口把本局 state 目录绑上去——它是应用私有、按款、可写的
//   持久目录（GameSession.prepareState 的 <saveRoot>/ryujinx-android），与 iOS
//   <Documents>/games/<gid> 同口径。必须在任何 s2a_docs_path 消费者（dguard 的
//   wire/direct_guard、patch 的 S2A/patches、envfile 的 env.txt 等）运行之前调；
//   环境是进程级的，不影响别的平台。
#pragma once

// state_dir 绑成 S2A_DOCS（覆盖写，同 launchctx 的 put 语义；只认绝对路径）。
// 0 = 成功；errno = 参数不是绝对路径或 setenv 失败。
int s2a_android_docs_bind(const char *state_dir);
