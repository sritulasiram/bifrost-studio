#include "bifrost/service/journal.hpp"

#include "bifrost/flash/flash.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace bifrost;
using namespace bifrost::service;

namespace {

class TempHome {
public:
    explicit TempHome(const std::string& prefix)
    {
        path_ = std::filesystem::temp_directory_path() / prefix;
        std::filesystem::create_directories(path_);
        setenv("HOME", path_.c_str(), 1);
    }
    ~TempHome()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        unsetenv("HOME");
    }

private:
    std::filesystem::path path_;
};

} // namespace

int main()
{
    {
        TempHome home("bifrost-journal-v2");
        JobSnapshot snapshot;
        snapshot.id = 7;
        snapshot.kind = JobKind::flash;
        snapshot.running = true;
        snapshot.progress.phase = flash::Phase::transferring;
        snapshot.progress.total = 5'000'000;
        snapshot.progress.totalDone = 2'500'000;
        snapshot.device = {0x04E8, 0x685D, 1, 4};
        snapshot.log.push_back({flash::LogLevel::info, "Sending AP: system.img.lz4"});
        journalPersist(snapshot);
        auto loaded = journalRecover();
        CHECK(loaded);
        CHECK(loaded->id == 7 && !loaded->running);
        CHECK(loaded->kind == JobKind::flash);
        CHECK(loaded->progress.total == 5'000'000 && loaded->progress.totalDone == 2'500'000);
        CHECK(loaded->progress.phase == flash::Phase::transferring);
        CHECK(loaded->device.vendorId == 0x04E8 && loaded->device.productId == 0x685D);
        CHECK(loaded->result && loaded->result->error);
        CHECK(loaded->result->error->code == ErrorCode::serviceUnavailable);
        CHECK(loaded->result->error->dataState == DataState::partial);
        CHECK(loaded->result->error->message.find("system.img.lz4") != std::string::npos);
        journalClear();
        CHECK(!journalRecover());
    }
    {
        TempHome home("bifrost-journal-finished");
        JobSnapshot snapshot;
        snapshot.id = 42;
        snapshot.kind = JobKind::probe;
        snapshot.running = false;
        journalPersist(snapshot);
        CHECK(!journalRecover());
    }
    return test::result();
}