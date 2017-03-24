#ifndef JERBOA_STATIC_KEYS_H
#define JERBOA_STATIC_KEYS_H

#include "core.h"

#define KEY(X) FastKey _skey_##X
#define KEY2(X,N) FastKey _skey_##N
#include "static_keys.txt"
#undef KEY
#undef KEY2

void initStaticKeys();

#endif
