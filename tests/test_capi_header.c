/* Compiles the public header as C and links against libbifrost.dylib. */
#include "bifrost/bifrost.h"

#include <string.h>

int main(void)
{
    bf_error error;
    bf_pit *pit;

    if (bf_api_version() != BF_API_VERSION) {
        return 1;
    }
    if (strlen(bf_version()) == 0) {
        return 1;
    }
    pit = bf_pit_parse(NULL, 0, &error);
    if (pit != NULL || error.code != BF_E_INVALID_ARGUMENT) {
        return 1;
    }
    return strcmp(bf_code_name(BF_E_PLAN_REFUSED), "unknown") == 0 ? 1 : 0;
}
