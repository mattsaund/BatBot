// SPDX-License-Identifier: MIT
//
// The entry point. Every case in the other test files registers itself at
// static-init time, so this does not need to know any of them by name.
#include <iostream>

#include "harness.hpp"

int main() {
    std::cout << "Crucible core tests\n\n";
    return harness::run_all();
}
