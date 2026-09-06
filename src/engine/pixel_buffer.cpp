#include "engine/pixel_buffer.h"

#include <cstring>

namespace alioth::engine {

void PixelBuffer::fill(std::uint8_t value) noexcept {
    if (data_) {
        std::memset(data_.get(), value, sizeBytes());
    }
}

}  // namespace alioth::engine
