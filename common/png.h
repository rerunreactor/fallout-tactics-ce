#ifndef FT_PNG_H
#define FT_PNG_H

#include <cstdint>
#include <string>
#include <vector>

namespace ft {

// Writes an 8-bit RGBA PNG.  `rgba` is width * height * 4 bytes, row-major.
bool WritePng(const std::string& path, int32_t width, int32_t height,
              const std::vector<uint8_t>& rgba, std::string* errorOut);

}  // namespace ft

#endif  // FT_PNG_H
