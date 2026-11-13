// libsecp256k1 (third_party/secp256k1, on the include path: v0.7.0 of github.com/bitcoin-core/secp256k1, MIT, only the files this needs) built
// as one unit for the chat (chat_crypto.cpp): Schnorr signatures, x-only keys and ECDH. Small tables (2 kB for signing,
// a window of 8 for verifying): a chat checks a few signatures a second at most.
#define SECP256K1_NO_API_VISIBILITY_ATTRIBUTES 1
#define ENABLE_MODULE_ECDH 1
#define ENABLE_MODULE_EXTRAKEYS 1
#define ENABLE_MODULE_SCHNORRSIG 1
#define ECMULT_WINDOW_SIZE 8
#define COMB_BLOCKS 2
#define COMB_TEETH 5
#include "src/secp256k1.c"
#include "src/precomputed_ecmult.c"
#include "src/precomputed_ecmult_gen.c"
