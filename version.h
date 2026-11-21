/* version.h - the one place mfm's version is set; mfm -v reports it.
 * MFM_GIT, from the Makefile, adds the commit built: +DATE.TIME.HASH. */
#ifndef VERSION_H
#define VERSION_H
#include "build/gitversion.h"
#define MFM_VERSION "0.2.7" MFM_GIT
#endif
