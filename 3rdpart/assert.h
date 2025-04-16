#ifndef ZLMEDIAKIT_ASSERT_H
#define ZLMEDIAKIT_ASSERT_H

#include <stdio.h>

#ifdef assert
    #undef assert
#endif//assert

#ifdef __cplusplus
extern "C" {
#endif
extern void Assert_Throw(int failed, const char *exp, const char *func, const char *file, int line, const char *str);
#ifdef __cplusplus
}
#endif

#define assert(exp) Assert_Throw(!(exp), #exp, __FUNCTION__, __FILE__, __LINE__, NULL)

#endif //ZLMEDIAKIT_ASSERT_H
