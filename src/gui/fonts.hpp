// SPDX-License-Identifier: MIT
//
// The typeface, as bytes.
//
// Generated at configure time by cmake/EmbedBinary.cmake from the JetBrains
// Mono release pinned in cmake/CrucibleDependencies.cmake. Declared here so
// theme.cpp does not have to; defined in build/generated/font_*.cpp, which is
// only compiled when the fetch succeeded -- CRUCIBLE_HAS_EMBEDDED_FONT is how
// the rest of the program finds out.
#pragma once

namespace crucible::gui::fonts {

extern const unsigned char kRegular[];
extern const unsigned int  kRegular_size;
extern const unsigned char kBold[];
extern const unsigned int  kBold_size;
extern const unsigned char kItalic[];
extern const unsigned int  kItalic_size;

}  // namespace crucible::gui::fonts
