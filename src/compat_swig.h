/* compat_swig.h — suppress fprintf in SWIG-generated wrapper.
 *
 * CRITICAL: this file must NOT be passed as -include to the OBJ_WRAP
 * compile rule.  The #define fprintf conflicts with `using ::fprintf`
 * from <cstdio> included transitively by the ER-301 SDK headers.
 * Include it directly only where needed. */
#pragma once
#define fprintf(stream, ...) ((void)0)
