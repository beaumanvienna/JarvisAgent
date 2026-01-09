#include <iostream>
#include "mylib.h"

int computeSum();

int main()
{
    std::cout << "x() = " << x() << "\n";
    std::cout << "y() = " << y() << "\n";
    std::cout << "sum = " << computeSum() << "\n";
    return 0;
}
