// FastCGI's records and connections (M10).
#include "cgi/fastcgi.h"

#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "doctest.h"

using namespace fourfold;
using namespace fourfold::fastcgi;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string begin(uint16_t id, bool keep, uint16_t role = kResponder) {
    std::string body(8, '\0');
    body[0] = static_cast<char>(role >> 8);
    body[1] = static_cast<char>(role & 0xFF);
    body[2] = keep ? static_cast<char>(kKeepConn) : '\0';
    return encodeRecord(kBeginRequest, id, body);
}

std::string request(uint16_t id, bool keep, const CgiVariables& vars, const std::string& body) {
    std::string out = begin(id, keep);
    out += encodeStream(kParams, id, encodePairs(vars));
    out += encodeStream(kStdin, id, body);
    return out;
}

// Runs serveConnection on one end of a socket pair; sends `input`, then
// (unless `keepOpen`) closes our sending side; returns all the records sent back.
std::vector<Record> converse(const std::string& input, const Handler& handler, CgiLimits limits = {}) {
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    Status result;
    std::thread server([&] { result = serveConnection(fds[1], handler, limits); ::shutdown(fds[1], SHUT_WR); });
    size_t at = 0;
    while (at < input.size()) at += static_cast<size_t>(::write(fds[0], input.data() + at, input.size() - at));
    ::shutdown(fds[0], SHUT_WR);
    RecordReader reader;
    std::vector<Record> records;
    char buffer[4096];
    while (true) {
        ssize_t n = ::read(fds[0], buffer, sizeof buffer);
        if (n <= 0) break;
        reader.feed(std::string_view(buffer, static_cast<size_t>(n)));
        while (auto r = reader.next().value()) records.push_back(*r);
    }
    server.join();
    ::close(fds[0]);
    ::close(fds[1]);
    CHECK_MESSAGE(result.ok(), (result.ok() ? "" : result.error().message));
    return records;
}

std::string stdoutOf(const std::vector<Record>& records, uint16_t id) {
    std::string out;
    for (const Record& r : records) {
        if (r.type == kStdout && r.requestId == id) out += r.content;
    }
    return out;
}

Handler echo = [](const CgiVariables& vars, std::string body) {
    return HttpResponse::text(200, vars.at("REQUEST_URI") + " " + body);
};

}  // namespace

TEST_CASE("FastCGI: records and pairs round-trip") {
    std::string longValue(300, 'v');
    CgiVariables vars{{"A", "1"}, {"LONG", longValue}, {"EMPTY", ""}};
    CgiVariables back;
    REQUIRE(decodePairs(encodePairs(vars), back));
    CHECK(back == vars);
    CHECK_FALSE(decodePairs(std::string("\x05\x01" "ab", 4), back));   // shorter than it says

    RecordReader reader;
    std::string bytes = encodeRecord(kStdin, 7, "hello");
    CHECK(bytes.size() == 16);   // padded to 8
    reader.feed(bytes.substr(0, 5));
    CHECK_FALSE(reader.next().value());
    reader.feed(bytes.substr(5));
    auto r = reader.next().value();
    REQUIRE(r);
    CHECK(r->type == kStdin);
    CHECK(r->requestId == 7);
    CHECK(r->content == "hello");
    RecordReader bad;
    bad.feed(std::string(8, '\x02'));
    CHECK_FALSE(bad.next().ok());

    std::string big(200000, 'x');
    RecordReader many;
    many.feed(encodeStream(kStdout, 1, big));
    std::string joined;
    int count = 0;
    while (auto rec = many.next().value()) {
        joined += rec->content;
        count++;
    }
    CHECK(joined == big);
    CHECK(count == 5);   // four with data, one empty
}

TEST_CASE("FastCGI: a request, and several on a kept connection") {
    auto records = converse(request(1, false, {{"REQUEST_URI", "/a"}, {"CONTENT_LENGTH", "4"}}, "body"), echo);
    std::string out = stdoutOf(records, 1);
    CHECK(out.rfind("Status: 200 OK\r\n", 0) == 0);
    CHECK(contains(out, "/a body"));
    REQUIRE_FALSE(records.empty());
    CHECK(records.back().type == kEndRequest);

    std::string two = request(1, true, {{"REQUEST_URI", "/one"}}, "") + request(2, true, {{"REQUEST_URI", "/two"}}, "");
    records = converse(two, echo);
    CHECK(contains(stdoutOf(records, 1), "/one"));
    CHECK(contains(stdoutOf(records, 2), "/two"));
}

TEST_CASE("FastCGI: limits, roles, multiplexing and management records") {
    int calls = 0;
    Handler counting = [&](const CgiVariables& v, std::string b) {
        calls++;
        return echo(v, std::move(b));
    };
    CgiLimits small;
    small.maxBodyBytes = 10;
    auto records = converse(request(1, false, {{"REQUEST_URI", "/"}, {"CONTENT_LENGTH", "20"}}, std::string(20, 'x')),
                            counting, small);
    CHECK(stdoutOf(records, 1).rfind("Status: 413", 0) == 0);
    // A body longer than its CONTENT_LENGTH.
    records = converse(request(1, false, {{"REQUEST_URI", "/"}, {"CONTENT_LENGTH", "2"}}, "four"), counting);
    CHECK(stdoutOf(records, 1).rfind("Status: 413", 0) == 0);
    CHECK(calls == 0);

    // Another role.
    records = converse(begin(3, false, 2), counting);
    REQUIRE(records.size() == 1);
    CHECK(records[0].type == kEndRequest);
    CHECK(records[0].content[4] == static_cast<char>(kUnknownRole));

    // A second request while one is open.
    std::string mpx = begin(1, true) + begin(2, true) + encodeStream(kParams, 1, encodePairs({{"REQUEST_URI", "/x"}})) +
                      encodeStream(kStdin, 1, "");
    records = converse(mpx, counting);
    bool refused = false;
    for (const Record& r : records) refused = refused || (r.type == kEndRequest && r.requestId == 2 && r.content[4] == static_cast<char>(kCantMpxConn));
    CHECK(refused);
    CHECK(contains(stdoutOf(records, 1), "/x"));

    // FCGI_GET_VALUES.
    records = converse(encodeRecord(kGetValues, 0, encodePairs({{"FCGI_MPXS_CONNS", ""}, {"FCGI_MAX_REQS", ""}})), counting);
    REQUIRE(records.size() == 1);
    CHECK(records[0].type == kGetValuesResult);
    CgiVariables values;
    REQUIRE(decodePairs(records[0].content, values));
    CHECK(values["FCGI_MPXS_CONNS"] == "0");
    CHECK(values["FCGI_MAX_REQS"] == "1");

    // An aborted request.
    records = converse(begin(4, false) + encodeRecord(kAbortRequest, 4, ""), counting);
    REQUIRE(records.size() == 1);
    CHECK(records[0].type == kEndRequest);
}
