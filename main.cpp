#include "lzss_test.h"

#include <iostream>

int main()
{
    return run_silesia_benchmark(std::cout, std::cerr) ? 0 : 1;
}
