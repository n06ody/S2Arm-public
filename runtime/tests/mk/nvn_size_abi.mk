# Authored IR -> real Analyze/Reflect/SPVC/Metal -> public NVN recording.
NATIVE_TEST_OWN += nvn_size_abi
ifeq ($(shell uname -s),Darwin)
NVN_SIZE_OUT := $(abspath $(NVN_TEST_DIR)/size-abi)
NVN_SIZE_TOOL := $(NVN_SIZE_OUT)/tool/dsh_cmp.dll
NVN_SIZE_ASSETS := $(NVN_SIZE_OUT)/assets
NVN_SIZE_BINARY ?= $(BUILD)/tests/t_nvn_size_abi
NVN_SIZE_TOOL_SOURCES := $(shell rg --files graphics/tools/direct/dsh translator/dmsh_xlate \
    graphics/widen/shader compat/tree/src/Ryujinx.Graphics.Shader \
    compat/tree/src/Spv.Generator compat/tree/src/Ryujinx.Common \
    -g '*.cs' -g '*.csproj' -g '*.glsl') graphics/widen/SpirvCbufUseText.cs \
    tests/direct_resolution_assets/Program.cs tests/direct_resolution_assets/DirectResolutionAssets.csproj \
    compat/tree/Directory.Build.props compat/tree/Directory.Packages.props
$(NVN_SIZE_TOOL): $(NVN_SIZE_TOOL_SOURCES) graphics/nvn/gfx/nvn/dmsh_chain.h
	@mkdir -p $(NVN_SIZE_OUT)
	dotnet build tests/direct_resolution_assets/DirectResolutionAssets.csproj -c Release -nodeReuse:false -p:UseSharedCompilation=false \
	  --artifacts-path $(NVN_SIZE_OUT)/dotnet -o $(NVN_SIZE_OUT)/tool \
	  -p:NuGetAudit=false -p:UseSharedCompilation=false -m:2 -v minimal
$(NVN_SIZE_ASSETS)/manifest.json: $(NVN_SIZE_TOOL)
	dotnet $(NVN_SIZE_TOOL) $(NVN_SIZE_ASSETS)
$(NVN_SIZE_BINARY): graphics/nvn/tests/t_nvn_size_abi.m $(NVN_MTL_DEPS) \
    $(wildcard graphics/nvn/gfx/nvn/*.h graphics/nvn/gfx/nvn/*.inc graphics/nvn/gfx/tex/*.h graphics/nvn/host/*.h graphics/nvn/host/*.inc) \
    graphics/nvn/texture_extent.inc graphics/nvn/widen_glue.inc graphics/nvn/widen_fallback.inc graphics/nvn/lib_pack.inc graphics/nvn/lib_pack_core.inc \
    graphics/metal/gal_samp_caps.h graphics/metal/fit_judge.h \
    $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) $(NVN_SIZE_ASSETS)/manifest.json
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -Wno-unused-function -Wno-unguarded-availability-new -Wno-unguarded-availability \
	  $(NATIVE_INC) -Igraphics/nvn/host -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/third_party/vulkan-headers/include \
	  -o $@ graphics/nvn/tests/t_nvn_size_abi.m $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -lc++
.PHONY: native-test-nvn_size_abi
native-test-nvn_size_abi: $(NVN_SIZE_BINARY)
	@for backend in mtl3 mtl4; do \
	  S2A_RES_SCALE=1 NXGAL_BE=$$backend tools/bin/timeout 60 $(NVN_SIZE_BINARY) $(NVN_SIZE_ASSETS) || exit $$?; \
	  for mode in wide128 wide131; do \
	    S2A_RES_SCALE=1 NXGAL_BE=$$backend tools/bin/timeout 60 $(NVN_SIZE_BINARY) $(NVN_SIZE_ASSETS) $$mode || exit $$?; \
	  done; \
	done
	# 延迟编码那一路（iOS 默认开编码线程，渲染编码器是录制替身 DmRecEnc）：11-01 族 36 新编码器方法缺替身、只在这路崩
	S2A_RES_SCALE=1 NXGAL_BE=mtl4 DM_NVN_ENC_THREAD=1 tools/bin/timeout 60 $(NVN_SIZE_BINARY) $(NVN_SIZE_ASSETS)
TEST_TARGETS += native-test-nvn_size_abi
endif
