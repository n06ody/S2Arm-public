// Host backend state and platform hooks for the actual policy and exit logger.
#pragma once
#include "nvn_vk_land_auto.h"
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include "nvn_plat.h"

static struct {
  int closing, land_auto, land_readback;
  unsigned land_prior_live, land_live_count;
  _Atomic int land_reported;
  struct la_trial land_trial;
} g;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static void capture_land_log(int level, const char* cls, const char* text);
#define nvn_plat_log capture_land_log
#include "../native/nvn/backend_vulkan_land_auto.inc"
#undef nvn_plat_log

static char decision[4096];
static unsigned lines;
static int thermal;
int dm_thermal_now(void) { return thermal; }
int s2a_android_host_lifecycle(uint64_t* life) { *life = 1; return 1; }
#ifndef LAND_TEST_LINKED_SHADER
uint64_t nvk_shader_live_compile_total(void) { return 0; }
#endif
static void capture_land_log(int level, const char* cls, const char* text) {
  if (level == NVN_PLAT_LOG_INFO && !strcmp(cls, "land-readback")) {
    snprintf(decision, sizeof decision, "%s", text);
    lines++;
  }
}
