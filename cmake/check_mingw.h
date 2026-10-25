/* Check's CMake probes find alarm's symbol without enabling its declaration.
 * Apply the documented MinGW feature gates, after Check's platform settings.
 * This header is forced into Check only; no guessed function prototypes.
 */
#include "config.h"
#ifndef _POSIX
#define _POSIX 1
#endif
#ifndef __USE_MINGW_ALARM
#define __USE_MINGW_ALARM 1
#endif
#include <io.h>
