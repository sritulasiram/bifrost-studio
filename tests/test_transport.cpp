// CDC Data interface selection: the only transport logic that can be tested
// without hardware.
#include "bifrost/transport/descriptors.hpp"
#include "support/test_support.hpp"

using bifrost::transport::AlternateSetting;
using bifrost::transport::TransferType;
using bifrost::transport::findOdinInterface;

int main()
{
    // Download Mode product IDs (Heimdall); Android mode (0x6860) is not one.
    CHECK(bifrost::transport::isDownloadModeProductId(0x685D));
    CHECK(bifrost::transport::isDownloadModeProductId(0x6601));
    CHECK(bifrost::transport::isDownloadModeProductId(0x68C3));
    CHECK(!bifrost::transport::isDownloadModeProductId(0x6860));
    CHECK(!bifrost::transport::isDownloadModeProductId(0x0000));

    const std::vector<AlternateSetting> valid{
        {0, 0, 0x02, 0, 0, {{0x83, TransferType::interrupt, 16}}},
        {1, 0, 0x0A, 0, 0,
         {{0x81, TransferType::bulk, 512}, {0x02, TransferType::bulk, 512}}},
    };
    const auto selected = findOdinInterface(valid);
    CHECK(selected && selected->interfaceNumber == 1 && selected->endpointIn == 0x81 &&
          selected->endpointOut == 0x02);

    // Not CDC Data.
    CHECK(!findOdinInterface({{0, 0, 0x02, 0, 0,
                               {{0x81, TransferType::bulk, 512},
                                {0x02, TransferType::bulk, 512}}}}));
    // Two IN endpoints, an interrupt endpoint, and a single endpoint.
    CHECK(!findOdinInterface({
        {1, 0, 0x0A, 0, 0, {{0x81, TransferType::bulk, 512}, {0x82, TransferType::bulk, 512}}},
        {2, 0, 0x0A, 0, 0, {{0x81, TransferType::bulk, 512}, {0x02, TransferType::interrupt, 16}}},
        {3, 0, 0x0A, 0, 0, {{0x81, TransferType::bulk, 512}}},
    }));
    return test::result();
}
