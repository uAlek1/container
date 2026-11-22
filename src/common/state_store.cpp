#include "state_store.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

namespace common {
namespace {

constexpr const char* kStateFileName = "state.pb";
constexpr const char* kTempFileName  = "state.pb.tmp";

}  // namespace

bool save_state(const std::filesystem::path& dir,
                const runtime::v1::ContainerStateFile& state,
                std::string& err) {
    err.clear();

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        err = "create_directories(" + dir.string() + "): " + ec.message();
        return false;
    }

    std::string body;
    if (!state.SerializeToString(&body)) {
        err = "SerializeToString failed";
        return false;
    }

    // Атомарная запись: пишем во временный файл, затем rename.
    // rename в пределах одной ФС атомарен, так что читатель либо увидит
    // старый state.pb, либо новый, но не полузаписанный.
    const auto tmp_path  = dir / kTempFileName;
    const auto final_path = dir / kStateFileName;

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "open(" + tmp_path.string() + "): " + std::strerror(errno);
            return false;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out) {
            err = "write(" + tmp_path.string() + "): " + std::strerror(errno);
            std::filesystem::remove(tmp_path, ec);  // убрать мусор
            return false;
        }
        out.flush();
        if (!out) {
            err = "flush(" + tmp_path.string() + "): " + std::strerror(errno);
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    }

    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) {
        err = "rename(" + tmp_path.string() + " -> " + final_path.string() +
              "): " + ec.message();
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    return true;
}

std::optional<runtime::v1::ContainerStateFile>
load_state(const std::filesystem::path& dir, std::string& err) {
    err.clear();

    const auto path = dir / kStateFileName;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "open(" + path.string() + "): " + std::strerror(errno);
        return std::nullopt;
    }

    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (in.bad()) {
        err = "read(" + path.string() + "): " + std::strerror(errno);
        return std::nullopt;
    }

    runtime::v1::ContainerStateFile state;
    if (!state.ParseFromString(body)) {
        err = "ParseFromString failed (" + path.string() + ")";
        return std::nullopt;
    }

    return state;
}

bool remove_state(const std::filesystem::path& dir, std::string& err) {
    err.clear();

    const auto path = dir / kStateFileName;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        err = "remove(" + path.string() + "): " + ec.message();
        return false;
    }

    // Убрать временный файл, если остался от упавшей записи.
    const auto tmp_path = dir / kTempFileName;
    std::filesystem::remove(tmp_path, ec);
    return true;
}

}  // namespace common