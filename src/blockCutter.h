#ifndef BLOCKCUTTER_H
#define BLOCKCUTTER_H

// Cuts a stream of arbitrarily sized chunks into blocks of EXACTLY `block` samples, for the audio
// thread: no allocation and no big memmove.
//
//  * whole blocks are handed to `onBlock(const int16_t *block)` straight from the input, nothing is copied;
//  * only the incomplete tail (always < block samples) is kept in `rest` until the next call.
//
// `rest` must have had rest.reserve(block) called once (Start()/init()), then it never reallocates.
// The pointer given to onBlock is only valid during that call.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

template<class OnBlock>
inline void cutBlocks(std::vector<int16_t> &rest, size_t block, const int16_t *in, size_t n, OnBlock &&onBlock)
{
    if(!block || !in)
        return;

    // 1) complete the partial block left over from the previous call
    if(!rest.empty()){
        const size_t take = std::min(block - rest.size(), n);
        rest.insert(rest.end(), in, in + take);
        in += take;
        n -= take;
        if(rest.size() < block)
            return;
        onBlock(rest.data());
        rest.clear();
    }

    // 2) whole blocks directly from the input
    while(n >= block){
        onBlock(in);
        in += block;
        n -= block;
    }

    // 3) keep the remainder (< block samples)
    if(n)
        rest.assign(in, in + n);
}

#endif // BLOCKCUTTER_H
