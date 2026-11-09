#include "docs_android.h"
#include <errno.h>
#include <stdlib.h>

int s2a_android_docs_bind(const char *state_dir) {
    if (!state_dir || state_dir[0] != '/') return EINVAL;
    if (setenv("S2A_DOCS", state_dir, 1)) return errno;
    return 0;
}
