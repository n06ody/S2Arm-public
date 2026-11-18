NATIVE_TEST_OWN += nvn_producer_barrier
ifeq ($(S2A_UNAME),Darwin)
NVN_PRODUCER_SOURCE ?= graphics/nvn/gfx/nvn/nvn_mtl.m
NVN_PRODUCER_BINARY ?= $(BUILD)/tests/t_nvn_producer_barrier
$(NVN_PRODUCER_BINARY): graphics/nvn/tests/t_nvn_producer_barrier.m $(NVN_PRODUCER_SOURCE) $(NVN_MTL_DEPS) $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -Wno-unused-function -Wno-unguarded-availability-new -Wno-unguarded-availability \
	  $(NATIVE_INC) -Igraphics/nvn/host -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/third_party/vulkan-headers/include \
	  -DNVN_PRODUCER_SOURCE='"$(abspath $(NVN_PRODUCER_SOURCE))"' \
	  -o $@ graphics/nvn/tests/t_nvn_producer_barrier.m $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen -Wl,-export_dynamic $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -framework Security -lc++
.PHONY: native-test-nvn_producer_barrier
native-test-nvn_producer_barrier: $(NVN_PRODUCER_BINARY)
	DM_NVN_PRODUCER_COVER=0 $(NVN_PRODUCER_BINARY)
	DM_NVN_PRODUCER_COVER=1 $(NVN_PRODUCER_BINARY)
	DM_NVN_ENC_THREAD=1 DM_NVN_PRODUCER_COVER=1 $(NVN_PRODUCER_BINARY)
	DM_NVN_ENC_THREAD=1 DM_NVN_PRODUCER_COVER=1 DM_NVN_BARRIER=game $(NVN_PRODUCER_BINARY)
	NXGAL_BE=mtl3 DM_NVN_PRODUCER_COVER=1 $(NVN_PRODUCER_BINARY)
TEST_TARGETS += native-test-nvn_producer_barrier
endif
