#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace okuflow::detail {
// Playback-only attenuation of whole PCM16 samples. Source PCM and timing stay unchanged.
inline void CopyPlaybackPcm16(void* destination, const void* source, std::size_t bytes, int percent) {
    percent=std::clamp(percent,0,100);
    if(percent==100) {std::memcpy(destination,source,bytes);return;}
    if(percent==0) {std::memset(destination,0,bytes);return;}
    auto* out=static_cast<unsigned char*>(destination);
    const auto* in=static_cast<const unsigned char*>(source);
    for(std::size_t at=0;at+sizeof(std::int16_t)<=bytes;at+=sizeof(std::int16_t)) {
        std::int16_t sample;std::memcpy(&sample,in+at,sizeof(sample));
        sample=static_cast<std::int16_t>(std::int32_t(sample)*percent/100);
        std::memcpy(out+at,&sample,sizeof(sample));
    }
}
}
