#include "lzss_test.h"

#include <iostream>

int main()
{
    return run_lzss_quickcheck(std::cout, std::cerr) ? 0 : 1;
}
