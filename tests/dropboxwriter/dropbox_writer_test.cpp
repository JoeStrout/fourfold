#include "dropboxwriter/dropbox_writer.h"

#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "doctest.h"
#include "platform/socket.h"
#include "temp_dir.h"

using namespace fourfold;
using fourfold::test::TempDir;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

RelPath rel(const std::string& s) { return RelPath::parse(s).value(); }

// A writer for `dir`/dropbox, listening on `dir`/w.sock, on a thread of its
// own.
struct Writer {
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<DropboxWriter> writer;
    std::thread thread;
    std::string socket;

    explicit Writer(const TempDir& dir) : socket(dir.at("w.sock")) {
        loop = EventLoop::create().value();
        Fd folder(::open(dir.at("dropbox").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        REQUIRE(folder.valid());
        writer = std::make_unique<DropboxWriter>(*loop, Dropbox(std::move(folder)));
        auto listening = listenUnix(socket);
        REQUIRE(listening.ok());
        REQUIRE(writer->adopt(std::move(listening).value()));
        thread = std::thread([this] { loop->run(); });
    }
    ~Writer() { stop(); }
    void stop() {
        if (!thread.joinable()) return;
        loop->stop();
        thread.join();
        writer.reset();
    }
};

// A raw exchange: `request` bytes in, the reply's bytes out (or "" on EOF).
std::string rawExchange(const std::string& socketPath, const std::string& request) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof addr.sun_path - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return "connect failed";
    }
    (void)!::write(fd, request.data(), request.size());
    std::string out;
    char buffer[4096];
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ssize_t n;
    while ((n = ::read(fd, buffer, sizeof buffer)) > 0) {
        out.append(buffer, static_cast<size_t>(n));
        if (out.size() >= 5) break;
    }
    ::close(fd);
    return out;
}

}  // namespace

TEST_CASE("DropboxClient and DropboxWriter: appends arrive, in order, through the writer") {
    TempDir dir;
    dir.makeDir("dropbox");
    Writer w(dir);
    DropboxClient client(w.socket);
    Dropbox box(std::make_shared<DropboxClient>(w.socket));
    REQUIRE(client.append(rel("app.log"), "one\n"));
    REQUIRE(box.append(rel("app.log"), "two\n"));
    REQUIRE(client.append(rel("sub/deep.log"), "x\n"));
    CHECK_EQ(slurp(dir.at("dropbox/app.log")), "one\ntwo\n");
    CHECK_EQ(slurp(dir.at("dropbox/sub/deep.log")), "x\n");
}

TEST_CASE("DropboxWriter: names the engine couldn't name are refused, with the reason") {
    TempDir dir;
    dir.makeDir("dropbox");
    Writer w(dir);
    for (const char* name : {"../escape.log", "", ".hidden", "a/../../b"}) {
        std::string reply = rawExchange(w.socket, encodeDropboxRequest(name, "x\n"));
        REQUIRE_GE(reply.size(), 5u);
        CHECK_EQ(static_cast<int>(reply[0]), static_cast<int>(ErrorCode::Invalid) + 1);
    }
    CHECK_EQ(rawExchange(w.socket, encodeDropboxRequest("ok.log", "fine\n")), encodeDropboxReply(Status()));
    CHECK_EQ(slurp(dir.at("dropbox/ok.log")), "fine\n");
    CHECK_FALSE(std::ifstream(dir.at("escape.log")).good());

    // Over the limit: refused, and the connection closed.
    std::string big = encodeDropboxRequest("big.log", "");
    big[4] = 0x7f;   // a text length far over the limit
    std::string reply = rawExchange(w.socket, big);
    REQUIRE_GE(reply.size(), 1u);
    CHECK_EQ(static_cast<int>(reply[0]), static_cast<int>(ErrorCode::TooLarge) + 1);
}

TEST_CASE("DropboxClient: reconnects when the writer restarts; fails clearly when it's down") {
    TempDir dir;
    dir.makeDir("dropbox");
    DropboxClient client(dir.at("w.sock"), 1000);
    auto down = client.append(rel("a.log"), "lost\n");
    REQUIRE_FALSE(down.ok());
    CHECK(down.error().message.find("dropbox writer") != std::string::npos);
    {
        Writer w(dir);
        REQUIRE(client.append(rel("a.log"), "1\n"));
    }
    {
        Writer w(dir);   // a new writer: the old connection is gone
        REQUIRE(client.append(rel("a.log"), "2\n"));
    }
    CHECK_EQ(slurp(dir.at("dropbox/a.log")), "1\n2\n");
    CHECK_FALSE(client.append(rel("a.log"), std::string(DropboxProtocolLimits::kMaxText + 1, 'x')).ok());
}
