/* byte_order.h : macros for dealing with byte order */
/* Originally PUBLIC DOMAIN 2004 - Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef BYTE_ORDER_H
#define BYTE_ORDER_H

#include <stdint.h>

#ifdef __linux__
# include <endian.h>
#elif !defined __BYTE_ORDER
# define __LITTLE_ENDIAN 1234
# define __BIG_ENDIAN 4321
# if defined __i386__ || defined __x86_64__ || defined __aarch64__ || \
     defined _M_IX86 || defined _M_X64 || defined _M_ARM64
#  define __BYTE_ORDER	__LITTLE_ENDIAN
# elif defined __POWERPC__ || defined __powerpc__ || defined __s390x__
#  define __BYTE_ORDER	__BIG_ENDIAN
# else
#  error could not guess endian
# endif
#endif

#ifndef __BYTE_ORDER
# error __BYTE_ORDER must be defined to be __LITTLE_ENDIAN or __BIG_ENDIAN
#endif

#if defined __GNUC__ && defined __linux__
# include <byteswap.h>
# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define LE16(x) (x)
#  define LE32(x) (x)
#  define LE64(x) (x)
#  define BE16(x) bswap_16((x))
#  define BE32(x) bswap_32((x))
#  define BE64(x) bswap_64((x))
# else
#  define LE16(x) bswap_16((x))
#  define LE32(x) bswap_32((x))
#  define LE64(x) bswap_64((x))
#  define BE16(x) (x)
#  define BE32(x) (x)
#  define BE64(x) (x)
# endif
#else
# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define LE16(x) (x)
#  define LE32(x) (x)
#  define LE64(x) (x)
#  define BE16(x) (((uint16_t)((x)<<8))|((uint16_t)(x)>>8))
#  define BE32(x) (((uint32_t)(BE16((x)))<<16)|(BE16((uint32_t)(x)>>16)))
#  define BE64(x) (((uint64_t)(BE32((x)))<<32)|(BE32((uint64_t)(x)>>32)))
# else
#  define LE16(x) (((uint16_t)((x)<<8))|((uint16_t)(x)>>8))
#  define LE32(x) (((uint32_t)(LE16((x)))<<16)|(LE16((uint32_t)(x)>>16)))
#  define LE64(x) (((uint64_t)(LE32((x)))<<32)|(LE32((uint64_t)(x)>>32)))
#  define BE16(x) (x)
#  define BE32(x) (x)
#  define BE64(x) (x)
# endif
#endif

#endif /* BYTE_ORDER_H */
