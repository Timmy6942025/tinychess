#pragma once

#ifdef USE_RUK_NET
constexpr int HIDDEN_SIZE = 512;  // RukChess 768->512->1 net (weights-ruk.cpp)
#elif defined(USE_RUK_NET_256)
constexpr int HIDDEN_SIZE = 256;  // RukChess 768->256->1 net (weights-ruk256.cpp)
#else
constexpr int HIDDEN_SIZE = 256;
#endif
