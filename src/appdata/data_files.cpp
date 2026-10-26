#include "appdata/data_files.h"

#include <algorithm>
#include <cmath>

#include "base/utf8.h"

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

Result<RelPath> DataFiles::checkPath(std::string_view path, bool allowRoot) {
    if (path.empty()) {
        if (allowRoot) return RelPath();
        return makeError(ErrorCode::Invalid, "a file path can't be empty");
    }
    if (path.front() == '/') {
        return makeError(ErrorCode::Invalid, "file paths are relative to data/, so they can't start with '/' (\"" +
                                                 std::string(path) + "\")");
    }
    auto parsed = RelPath::parse(path);
    if (!parsed) return makeError(ErrorCode::Invalid, "bad file path \"" + std::string(path) + "\": " +
                                                          parsed.error().message);
    return parsed;
}

std::string DataFiles::lockKey(const RelPath& file) {
    std::string key = file.str();
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
}

std::string DataFiles::valueLockKey(const RelPath& name) { return lockKey(valuePath(name)); }

RelPath DataFiles::valuePath(const RelPath& name) {
    return RelPath::parse(name.str() + ".json").value();
}

RelPath DataFiles::logPath(const RelPath& name) {
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

Result<std::optional<std::string>> DataFiles::readFile(const RelPath& path) const {
    auto bytes = dir_.read(path, limits_.maxBytes);
    if (!bytes) {
        if (bytes.error().code == ErrorCode::NotFound) {
            // A folder isn't a missing file: say so.
            auto i = dir_.info(path);
            if (i && i.value() && i.value()->isFolder) {
                return makeError(ErrorCode::Conflict, "data/" + path.str() + " is a folder");
            }
            return std::optional<std::string>();
        }
        return bytes.error();
    }
    return std::optional<std::string>(std::move(bytes).value());
}

Status DataFiles::writeFile(const RelPath& path, std::string_view bytes) const {
    if (bytes.size() > limits_.maxBytes) {
        return makeError(ErrorCode::TooLarge, "data/" + path.str() + ": over " + std::to_string(limits_.maxBytes) +
                                                  " bytes");
    }
    return dir_.write(path, bytes);
}

Result<bool> DataFiles::removeFile(const RelPath& path) const {
    auto i = dir_.info(path);
    if (!i) return i.error();
    if (!i.value()) return false;
    Status s = i.value()->isFolder ? dir_.removeFolder(path) : dir_.remove(path);
    if (!s) {
        if (s.error().code == ErrorCode::NotFound) return false;   // removed meanwhile
        return s.error();
    }
    return true;
}

Status DataFiles::makeFolder(const RelPath& path) const { return dir_.makeFolder(path); }

Result<std::optional<FileInfo>> DataFiles::info(const RelPath& path) const { return dir_.info(path); }

Result<std::vector<std::string>> DataFiles::children(const RelPath& folder) const {
    auto i = dir_.info(folder);
    if (!i) return i.error();
    if (!i.value()) return makeError(ErrorCode::NotFound, "data/" + folder.str() + ": not found");
    if (!i.value()->isFolder) return makeError(ErrorCode::Conflict, "data/" + folder.str() + " isn't a folder");
    auto entries = dir_.list(folder);
    if (!entries) return entries.error();
    std::vector<std::string> out;
    for (const DirEntry& e : entries.value()) out.push_back(e.name);
    return out;
}

Result<std::vector<std::string>> DataFiles::splitLines(std::string_view bytes) {
    if (bytes.find('\0') != std::string_view::npos || !isValidUtf8(bytes)) {
        return makeError(ErrorCode::Invalid, "isn't UTF-8 text (use file.loadRaw for binary files)");
    }
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < bytes.size(); i++) {
        char c = bytes[i];
        if (c != '\n' && c != '\r') continue;
        lines.emplace_back(bytes.substr(start, i - start));
        if (c == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') i++;
        start = i + 1;
    }
    if (start < bytes.size()) lines.emplace_back(bytes.substr(start));
    return lines;
}

std::string DataFiles::joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

}  // namespace fourfold
