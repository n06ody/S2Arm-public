// SPDX-License-Identifier: Apache-2.0
// With -Dlink=s2a_denied_link, unistd.h supplies the real host API signature.
// Model Android's app SELinux link denial even when the host filesystem allows it.
#include <unistd.h>
#include <errno.h>
int s2a_denied_link(const char *old_path, const char *new_path) {
    (void)old_path; (void)new_path; errno = EPERM; return -1;
}
