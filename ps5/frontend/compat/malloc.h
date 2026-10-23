// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/* PS5: the SDK's <malloc.h> is an #error; its contents are in <stdlib.h>. */
#ifndef PS5PX_COMPAT_MALLOC_H
#define PS5PX_COMPAT_MALLOC_H
#include <stdlib.h>
#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif
#endif
