#include <string.h>

#include "static_keys.h"
#include "hash.h"

void initStaticKeys() {
#define KEY(X) _skey_##X = prepare_key(#X, strlen(#X))
#define KEY2(X, N) _skey_##N = prepare_key(#X, strlen(#X))
#include "static_keys.txt"
#undef KEY
}
