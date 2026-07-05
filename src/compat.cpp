/* compat.cpp — Platform stubs for arm-none-eabi newlib on ER-301.
 *
 * sincosf is called internally by some math functions on Cortex-A8 newlib.
 * Without this stub, the linker produces an undefined reference.
 * This file MUST be compiled WITHOUT -ffast-math (see Makefile CXXFLAGS_COMPAT).
 *
 * fprintf is #defined to a no-op in compat_swig.h.  That header is NOT
 * used as a -include flag on the SWIG wrap compilation — doing so would
 * conflict with <cstdio>'s `using ::fprintf` declaration. */

#include <math.h>
#include <stdio.h>

void sincosf(float x, float *s, float *c)
{
    *s = sinf(x);
    *c = cosf(x);
}
