// file.h -- ReadWholeFile, shared by sprview and mapview.
//
// This was defined twice, once in sprview/spr.cpp and once in mapview/til.cpp,
// with the two copies declared in spr.h and til.h respectively.  Milestone 3
// links spr.o into mapview, so the two definitions would now collide; the
// function moved here and both declarations now forward to it.

#ifndef FT_FILE_H
#define FT_FILE_H

#include <cstdint>
#include <string>
#include <vector>

namespace ft {

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out, std::string* errorOut);

}  // namespace ft

#endif  // FT_FILE_H
