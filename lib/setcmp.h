#ifndef _SETCMP_H__
#define _SETCMP_H__
#include "lib/bitmap.h"
#include "lib/khash64.h"
#include "lib/feature_min.h"

namespace emp {


template<typename KhashType>
size_t intersection_size(const KhashType *a, const KhashType *b) {
    size_t ret(0);
    for(khiter_t ki(0); ki != kh_end(a); ++ki)
        if(kh_exist(a, ki))
            ret += (khash_get(b, kh_key(a, ki)) != kh_end(b));
    return ret;
}

template<typename KhashType>
double jaccard_index(const KhashType *a, const KhashType *b) {
    const auto is(intersection_size(a, b));
    return static_cast<double>(is) / (kh_size(a) + kh_size(b) - is);
}

} // namespace emp


#endif // #ifndef _SETCMP_H__