#include "plan/int63.h"

__extension__ typedef __int128 pl_int128;

pl_val pl_int63_muldiv(pl_val a, pl_val b, pl_val divisor,
                       pl_int63_round rounding) {
  pl_int128 product = (pl_int128)pl_int63_decode(a) * pl_int63_decode(b);
  int64_t d = pl_int63_decode(divisor);
  pl_int128 quotient = product / d;
  int64_t remainder = (int64_t)(product % d);
  if (remainder != 0) {
    bool negative = (product < 0) != (d < 0);
    switch (rounding) {
    case PL_INT63_ROUND_ZERO:
      break;
    case PL_INT63_ROUND_FLOOR:
      quotient -= negative;
      break;
    case PL_INT63_ROUND_CEIL:
      quotient += !negative;
      break;
    case PL_INT63_ROUND_EVEN: {
      uint64_t twice = (uint64_t)(remainder < 0 ? -remainder : remainder) * 2;
      uint64_t magnitude = (uint64_t)(d < 0 ? -d : d);
      if (twice > magnitude || (twice == magnitude && quotient % 2 != 0))
        quotient += negative ? -1 : 1;
      break;
    }
    }
  }
  return pl_int63_from_bits((uint64_t)quotient);
}
