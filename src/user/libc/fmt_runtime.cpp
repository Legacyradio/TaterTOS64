/*
 * TaterTOS64v3 — fmt runtime translation unit (PATCHED).
 */

#define FMT_HEADER_ONLY 1
#define FMT_USE_LOCALE 0
#define FMT_USE_FCNTL 0

/* Suppress C++23 fixed-width float types */
#undef __STDCPP_FLOAT64_T__
#undef __STDCPP_FLOAT32_T__
#undef __STDCPP_FLOAT16_T__
#undef __STDCPP_BFLOAT16_T__

#include <fmt/format.h>

namespace fmt {
    namespace v12 {
        namespace detail {
            namespace dragonbox {
                /* Explicitly instantiate to_decimal to avoid link errors in AK/StringConversions.o */
                template decimal_fp<float> to_decimal(float);
                template decimal_fp<double> to_decimal(double);
            }
        }
    }
}
