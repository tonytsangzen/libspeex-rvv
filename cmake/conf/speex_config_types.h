/* Hand-written replacement for configure's speex_config_types.h.
 * Valid for any LP64/ILP32 target with <stdint.h> (riscv64 linux,
 * macOS arm64, ...). speex_types.h includes this file as
 * "speex_config_types.h"; keep the guard identical to the template. */
#ifndef __SPEEX_TYPES_H__
#define __SPEEX_TYPES_H__

#include <stdint.h>

typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;

#endif /* __SPEEX_TYPES_H__ */
