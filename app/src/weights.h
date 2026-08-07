#pragma once

#ifdef USE_RUK_NET
constexpr int HIDDEN_SIZE = 512;  // RukChess 768->512->1 net (weights-ruk.cpp)
#else
constexpr int HIDDEN_SIZE = 256;
#endif
