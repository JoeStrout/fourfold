#include "appdata/data_files.h"

#include <algorithm>
#include <cmath>

namespace fourfold {

namespace {

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Error conflict(const RelPath& name, const char* is, const char* use) {
    return makeError(ErrorCode::Conflict,
                     "\"" + name.str() + "\" is " + is + "; use " + use + " with it, or another name");
}

}  // namespace

DataFiles::DataFiles(DataDir dir, DataLimits limits) : dir_(std::move(dir)), limits_(limits) {
    lockWaiters_ = std::make_unique<WorkerPool>(2);
    locks_ = makeFileLockService([this](const std::string& key) { return dir_.lockFile(key); }, *lockWaiters_);
}

DataFiles::~DataFiles() {
    locks_.reset();   // waits for any lock-file waits in progress
    lockWaiters_.reset();
}

Result<RelPath> DataFiles::checkName(std::string_view name) {
    if (name.empty()) return makeError(ErrorCode::Invalid, "a data name can't be empty");
    if (name.back() == '/') return makeError(ErrorCode::Invalid, "a data name can't end with '/'");
    if (endsWith(name, ".json") || endsWith(name, ".jsonl")) {
        return makeError(ErrorCode::Invalid, "leave the extension off a data name (\"guestbook\", not "
                                             "\"guestbook.json\")");
    }
    auto path = RelPath::parse(name);
    if (!path) return makeError(ErrorCode::Invalid, "bad data name \"" + std::string(name) + "\": " +
                                                        path.error().message);
    // Room for the extension.
    if (path.value().name().size() + 6 > RelPath::kMaxSegmentBytes) {
        return makeError(ErrorCode::Invalid, "data name too long");
    }
    return path;
}

Result<RelPath> DataFiles::checkFolder(std::string_view folder) {
    if (folder.empty()) return RelPath();
    if (folder.back() == '/') folder.remove_suffix(1);
    auto path = RelPath::parse(folder);
    if (!path) return makeError(ErrorCode::Invalid, "bad data folder \"" + std::string(folder) + "\": " +
                                                        path.error().message);
    return path;
}

RelPath DataFiles::valuePath(const RelPath& name) const {
    return RelPath::parse(name.str() + ".json").value();
}

RelPath DataFiles::logPath(const RelPath& name) const {
    return RelPath::parse(name.str() + ".jsonl").value();
}

Result<std::optional<Datum>> DataFiles::readValue(const RelPath& name) const {
    auto text = dir_.read(valuePath(name), limits_.maxBytes);
    if (!text) {
        if (text.error().code == ErrorCode::NotFound) return std::optional<Datum>();
        return text.error();
    }
    JsonLimits json;
    json.maxBytes = limits_.maxBytes;
    auto value = parseJson(text.value(), json);
    if (!value) return makeError(ErrorCode::Invalid, "data/" + valuePath(name).str() + ": " + value.error().message);
    return std::optional<Datum>(std::move(value).value());
}

Result<std::optional<Datum>> DataFiles::get(const RelPath& name) const {
    auto value = readValue(name);
    if (!value || value.value()) return value;

    auto text = dir_.read(logPath(name), limits_.maxBytes);
    if (!text) {
        if (text.error().code == ErrorCode::NotFound) return std::optional<Datum>();
        return text.error();
    }
    // One value per line. A last line with no newline was cut off part way
    // (a crash during an append), and is left out.
    const std::string& s = text.value();
    Datum::ListItems items;
    size_t start = 0;
    int lineNumber = 1;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) break;
        std::string_view line(s.data() + start, end - start);
        if (!line.empty()) {
            auto item = parseJson(line);
            if (!item) {
                return makeError(ErrorCode::Invalid, "data/" + logPath(name).str() + " line " +
                                                         std::to_string(lineNumber) + ": " + item.error().message);
            }
            items.push_back(std::move(item).value());
        }
        start = end + 1;
        lineNumber++;
    }
    return std::optional<Datum>(Datum::list(std::move(items)));
}

Status DataFiles::set(const RelPath& name, const Datum& value) const {
    auto log = dir_.exists(logPath(name));
    if (!log) return log.error();
    if (log.value()) return conflict(name, "a log (made by data.append)", "data.append and data.get");
    std::string json = toJson(value);
    if (json.size() > limits_.maxBytes) {
        return makeError(ErrorCode::TooLarge, "the value for \"" + name.str() + "\" is over " +
                                                  std::to_string(limits_.maxBytes) + " bytes");
    }
    return dir_.write(valuePath(name), json);
}

Status DataFiles::append(const RelPath& name, const Datum& value) const {
    auto plain = dir_.exists(valuePath(name));
    if (!plain) return plain.error();
    if (plain.value()) return conflict(name, "a value (made by data.set)", "data.set, data.get and data.update");
    std::string line = toJson(value) + "\n";   // JSON never contains a raw newline
    if (line.size() > limits_.maxBytes) {
        return makeError(ErrorCode::TooLarge, "the entry for \"" + name.str() + "\" is over " +
                                                  std::to_string(limits_.maxBytes) + " bytes");
    }
    return dir_.append(logPath(name), line);
}

Result<bool> DataFiles::remove(const RelPath& name) const {
    bool removed = false;
    for (const RelPath& path : {valuePath(name), logPath(name)}) {
        Status s = dir_.remove(path);
        if (s) removed = true;
        else if (s.error().code != ErrorCode::NotFound) return s.error();
    }
    return removed;
}

Result<bool> DataFiles::exists(const RelPath& name) const {
    for (const RelPath& path : {valuePath(name), logPath(name)}) {
        auto e = dir_.exists(path);
        if (!e) return e.error();
        if (e.value()) return true;
    }
    return false;
}

Result<std::vector<std::string>> DataFiles::names(const RelPath& folder) const {
    auto entries = dir_.list(folder);
    if (!entries) return entries.error();
    std::vector<std::string> out;
    for (const DirEntry& e : entries.value()) {
        if (e.isFolder) {
            out.push_back(e.name + "/");
        } else if (endsWith(e.name, ".jsonl")) {
            out.push_back(e.name.substr(0, e.name.size() - 6));
        } else if (endsWith(e.name, ".json")) {
            out.push_back(e.name.substr(0, e.name.size() - 5));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Result<double> DataFiles::increment(const RelPath& name, double by) const {
    auto current = readValue(name);
    if (!current) return current.error();
    double n = 0;
    if (current.value()) {
        if (current.value()->kind() != Datum::Kind::Number) {
            return makeError(ErrorCode::Conflict, "\"" + name.str() + "\" isn't a number");
        }
        n = current.value()->asNumber();
    } else {
        auto log = dir_.exists(logPath(name));
        if (!log) return log.error();
        if (log.value()) return conflict(name, "a log (made by data.append)", "data.append and data.get");
    }
    n += by;
    if (!std::isfinite(n)) return makeError(ErrorCode::Invalid, "\"" + name.str() + "\" would overflow");
    if (Status s = dir_.write(valuePath(name), toJson(Datum::number(n))); !s) return s.error();
    return n;
}

Result<bool> DataFiles::compareAndSet(const RelPath& name, const Datum& expected, const Datum& replacement) const {
    auto current = readValue(name);
    if (!current) return current.error();
    const Datum& now = current.value() ? *current.value() : Datum();
    if (toJson(now) != toJson(expected)) return false;
    if (Status s = set(name, replacement); !s) return s.error();
    return true;
}

}  // namespace fourfold
