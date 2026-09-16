#ifndef _LOS_VERSION_H
#define _LOS_VERSION_H

#define LOS_NAME      "LightningOS"
#define LOS_VERSION   "2.0"
#define LOS_CODENAME  "Thunderbolt"
#define LOS_ARCH      "i386"

/* build.py passes -DLOS_BUILD_STAMP=<date>. Using __DATE__ instead would make
   the build non-reproducible, which clang rejects by default under zig cc. */
#define _LOS_STRINGIFY(x) #x
#define _LOS_STR(x) _LOS_STRINGIFY(x)

#ifdef LOS_BUILD_STAMP
#define LOS_BUILD _LOS_STR(LOS_BUILD_STAMP)
#else
#define LOS_BUILD "unknown"
#endif

#endif
