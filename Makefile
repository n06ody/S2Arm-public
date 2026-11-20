# S2Arm 顶层构建入口。各目录在自己的 <目录>/build.mk 里往下列变量追加，顶层不写任何目录的细节：
#   HOST_SRCS   进 Mac 宿主 / iOS App 的源文件        HOST_LIBS  链接参数
#   TEST_TARGETS  `make test` 要跑的目标（每个目标自己编、自己跑、失败即非 0）
# 常用：make host（Mac 宿主 build/s2arm-host）、make test（全部单测）
# 统一工具链（tools/toolchain.env）：配方里的 clang / xcrun 都跟它走；$(shell xcrun …) 处显式带 DEVELOPER_DIR=$(DEVELOPER_DIR)（make 3.81 的 $(shell) 不继承 export）
# Xcode 按用户机器（10-25，不写死版本）：指定 S2A_DEVELOPER_DIR > 环境里的 DEVELOPER_DIR（tools/toolchain.sh 解析后导出）> tools/toolchain.sh 当场解析（缺省装着的最新 Xcode，11-07 Duo 定因）> xcode-select；
#   先在 include 之前记下环境值——toolchain.env 里那行不当缺省（make 里文件内赋值会盖过环境变量）。赋值行不带行尾注释：注释前的空格会进值
_S2A_ENV_DD := $(DEVELOPER_DIR)
include tools/toolchain.env
# 配方失败即删掉目标（10-24 着色器线：gfx 门禁「首跑红、重跑绿」是真失败被增量构建盖住——失败的配方先写出了目标，重跑时 make 判它已最新就跳过）。
#   */build.mk 都 include 进本文件，这一行覆盖它们；graphics/metal、graphics/nvn 下单独的 Makefile 各自另加
.DELETE_ON_ERROR:
# 宿主系统名只取一次（11-14 门禁提速）：*/build.mk 与单测分片两百多处 ifeq 判 Darwin，原先各写一次 $(shell uname -s)，每次 make 起两百多个进程；
#   原生层门禁每组一次 make、350 组，满载机器上单次解析 4–6 秒大半耗在这里。新分片照写 $(S2A_UNAME)（写 $(shell uname -s) 也对，只是慢）
S2A_UNAME := $(shell uname -s)
# 前端 / 玩家机器用 S2A_DEVELOPER_DIR 指自己的 Xcode（同 tools/toolchain.sh，#49）：不设 = toolchain.env 那套（10-23 Spare-host 全新机器实测：Makefile 不认，没装 27.1 的机器着色器键算不出）
DEVELOPER_DIR := $(or $(S2A_DEVELOPER_DIR),$(_S2A_ENV_DD),$(shell S2A_TC_QUIET=1 bash -c '. tools/toolchain.sh > /dev/null && printf %s "$$DEVELOPER_DIR"' 2>/dev/null),$(shell xcode-select -p 2>/dev/null))
export DEVELOPER_DIR MACOSX_DEPLOYMENT_TARGET MSBUILDDISABLENODEREUSE   # iOS 下限不导出（两个都导出时不带 -target 的宿主编译会当成 iOS，见 tools/toolchain.sh）
# Apple 移动端出哪个平台（11-15 Apple TV 版）：ios（缺省）| tvos。`make ios S2A_APPLE_OS=tvos` 出 Apple TV 壳，与 iOS 同一套配方，
#   只换 SDK / 目标三元组 / 系统下限 / C# 运行时标识 / 渲染器库产物目录；没显式给 BUILD 时 tvOS 产物放 build/tvos，与 iOS 的 build/ 互不覆盖
S2A_APPLE_OS ?= ios
ifeq ($(S2A_APPLE_OS),tvos)
BUILD ?= build/tvos
APPLE_SDK := appletvos
APPLE_MIN := $(TVOS_DEPLOYMENT_TARGET)
APPLE_MINFLAG := -mtvos-version-min=$(TVOS_DEPLOYMENT_TARGET)
APPLE_TRIPLE := arm64-apple-tvos$(TVOS_DEPLOYMENT_TARGET)
APPLE_RID := tvos-arm64
APPLE_GFX_OUT := -tvos
APPLE_PLATFORM := AppleTVOS
APPLE_DEVICE_FAMILY := <array><integer>3</integer></array>
else ifeq ($(S2A_APPLE_OS),ios)
APPLE_SDK := iphoneos
APPLE_MIN := $(IPHONEOS_DEPLOYMENT_TARGET)
APPLE_MINFLAG := -mios-version-min=$(IPHONEOS_DEPLOYMENT_TARGET)
APPLE_TRIPLE := arm64-apple-ios$(IPHONEOS_DEPLOYMENT_TARGET)
APPLE_RID := ios-arm64
APPLE_GFX_OUT :=
APPLE_PLATFORM := iPhoneOS
APPLE_DEVICE_FAMILY := <array><integer>1</integer><integer>2</integer></array>
else
$(error S2A_APPLE_OS 只认 ios / tvos，给的是 $(S2A_APPLE_OS))
endif
BUILD   ?= build
CC      ?= clang
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
HOST_SRCS :=
HOST_LIBS :=
TEST_TARGETS :=
# dav1d 是 AV1 软件兜底（只有没有 AV1 硬件解码的机器才用到）：S2A_DAV1D = auto（显式设）= 探测本机（tools/av1hw.sh：VideoToolbox
#   建得出 AV1 解码会话 = 有硬解 → AV1 走硬解路线，dav1d 静态库 / 预编库 / meson / ninja 依赖整条不进构建，见 services/build.mk、
#   launchers/macos/prepare_build.py；没有硬解的机器 M1 / M2、A16 及更早与原默认一致，照编照链）；1 = 强制链（出包会落到
#   没有硬解的机器上时——比如发版机出的 App）；0 = 强制不链。
# S2A_MAKE_DRY 从 services/build.mk 挪上来：auto 的解析在 include 之前就要用（空跑 make -n 不出编译器真探测，按要链算）
S2A_MAKE_DRY := $(findstring n,$(firstword -$(MAKEFLAGS)))
# 缺省 1（链）：构建机出的二进制会分发到别的机器（发版的 Mac 宿主、iOS 库、启动器随包），探测构建机判不了玩家设备；
#   auto 只在「构建出来只给本机自己跑」时显式设（本地开发省依赖）
S2A_DAV1D ?= 1
# 生效值（1 = 链 dav1d），进 HOST_CFG：机器换了 / 探测脚本改得翻转结果时重链。$(shell) 显式带 DEVELOPER_DIR 与 S2A_DAV1D
#   （make 3.81 的 $(shell) 不继承 export 见上；S2A_DAV1D 不总在环境里，命令行给的值要自己带进子 shell）；探不出来按要链算
S2A_DAV1D_EFF := $(if $(S2A_MAKE_DRY),1,$(shell DEVELOPER_DIR=$(DEVELOPER_DIR) S2A_DAV1D=$(S2A_DAV1D) bash tools/av1hw.sh --want 2>/dev/null || echo 1))
# libvpx 同理（VP8 / VP9 软解）：S2A_VPX = auto（显式设）= 探测本机可走系统 VP9（tools/vp9hw.sh：注册 supplemental 后真建一次会话，
#   与运行时同一条判据 #348）→ VP9 走 VideoToolbox，libvpx 静态库 / 预编库整条不进构建；VP8 无任何系统 / 托管替代（VideoToolbox
#   不解 VP8），没链 libvpx 的机器上 VP8 初始化报红不出假画面；出包要落到这些内容上时（发版机、iPhone 包）设 S2A_VPX=1
S2A_VPX ?= 1   # 同上：缺省链，auto 只给本机自用的构建显式设
S2A_VPX_EFF := $(if $(S2A_MAKE_DRY),1,$(shell DEVELOPER_DIR=$(DEVELOPER_DIR) S2A_VPX=$(S2A_VPX) bash tools/vp9hw.sh --want 2>/dev/null || echo 1))
.DEFAULT_GOAL := host
-include $(sort $(wildcard */build.mk))

# 宿主依赖在全部 build.mk 读完之后才写：规则的依赖在读入那一刻展开，写在 runtime/build.mk 里时按字母序排在后面的 services/、tests/ 追加的源文件
# （自动玩家等）不在依赖里，改了 make host 不重编（10-20 服务接入线实测作废两局暗黑2、原生层一局八方2）。
# 构建配置戳：S2A_CS / S2A_PGO_GEN / S2A_DAV1D 与 S2A_VPX（生效值）/ CC / CFLAGS 变了也要重编（先不带 C# 编、再带 C# 编之类）；内容不变就不改写，不触发重编
HOST_CFG     := S2A_CS=$(S2A_CS) S2A_PGO_GEN=$(S2A_PGO_GEN) S2A_DAV1D=$(S2A_DAV1D_EFF) S2A_VPX=$(S2A_VPX_EFF) S2A_MAC_FRONTEND=$(S2A_MAC_FRONTEND) CC=$(CC) CFLAGS=$(CFLAGS)
HOST_CFG_STAMP := $(BUILD)/.host_cfg
$(shell mkdir -p "$(BUILD)"; [ "$$(cat "$(HOST_CFG_STAMP)" 2>/dev/null)" = '$(HOST_CFG)' ] || printf '%s' '$(HOST_CFG)' > "$(HOST_CFG_STAMP)")
$(BUILD)/s2arm-host: $(HOST_SRCS) $(wildcard $(addsuffix *.h,$(sort $(dir $(HOST_SRCS)))) runtime/include/*.h) $(HOST_CFG_STAMP) Makefile $(wildcard */build.mk)

.PHONY: host test clean
host: $(BUILD)/s2arm-host
test: $(TEST_TARGETS)
	@echo "全部单测通过（$(words $(TEST_TARGETS)) 组）"
clean:
	rm -rf $(BUILD)
