#include "glr_forest_check.hpp"
#include <iostream>

int main()
{
    checkForestEdits({"", "a b c", "a a b c", "a a b c b b", "a b c b b",
                      "a b c", "", "", "a b b c", "a b c", "@", ""});
    std::cout << "Nullable forest repair checks passed\n";
}
