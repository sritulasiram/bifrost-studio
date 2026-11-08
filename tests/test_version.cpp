#include "bifrost/version.h"

#include <string_view>

int main()
{
    return std::string_view(bifrost_core_version()) == "1.0.0" ? 0 : 1;
}
