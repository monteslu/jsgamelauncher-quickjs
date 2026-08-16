/*
 * Drop-in stand-in for node-addon-api's <napi.h>.
 *
 * native-gles's gl_bindings.h opens with `#include <napi.h>`. Rather than patch that
 * file (which would fork it and put us on the hook for every upstream change), we
 * put THIS napi.h earlier on the include path. gl_bindings.cpp then compiles
 * verbatim against the QuickJS shim.
 *
 * That "verbatim" property is the whole point: native-gles stays a normal upstream
 * dependency, and a bump does not become a merge.
 */
#ifndef JSGLQ_NAPI_H
#define JSGLQ_NAPI_H
#include "napi_shim.h"
#endif
