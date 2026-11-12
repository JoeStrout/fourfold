// `fourfold cgi-setup`: makes a web root serve an app through CGI or FastCGI
// (overview §8, Hosting modes; M10).

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "cgi.h"
#include "partition/app_folder.h"

namespace fourfold::cli {

namespace {

namespace fs = std::filesystem;

const char* const kMarker = "# Written by fourfold cgi-setup";

std::string realPath(const std::string& path) {
    char buffer[PATH_MAX];
    if (!::realpath(path.c_str(), buffer)) return {};
    return buffer;
}

// This program's own path.
std::string executablePath() {
#ifdef __APPLE__
    char buffer[PATH_MAX];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0) return realPath(buffer);
    return {};
#else
    return realPath("/proc/self/exe");
#endif
}

// 'it''s' for sh.
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
}

bool writtenByUs(const std::string& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.find(kMarker) != std::string::npos;
}

bool writeFile(const std::string& path, const std::string& text, mode_t mode, std::string& error) {
    std::string temp = path + ".fourfold-tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            error = "can't write " + path;
            return false;
        }
    }
    if (::chmod(temp.c_str(), mode) != 0 || ::rename(temp.c_str(), path.c_str()) != 0) {
        ::unlink(temp.c_str());
        error = "can't write " + path;
        return false;
    }
    return true;
}

}  // namespace

std::string cgiEntryScript(const std::string& binary, const std::string& appRoot, bool fastcgi) {
    return std::string("#!/bin/sh\n") + kMarker +
           ": every request to this site goes to Fourfold.\n"
           "exec " + shellQuote(binary) + (fastcgi ? " fastcgi " : " cgi ") + shellQuote(appRoot) + "\n";
}

std::string cgiHtaccess(bool fastcgi) {
    std::string entry = fastcgi ? "fourfold.fcgi" : "fourfold.cgi";
    std::string escaped = fastcgi ? "fourfold\\.fcgi" : "fourfold\\.cgi";
    return std::string(kMarker) +
           ".\n"
           "# Every request goes to Fourfold, static files included, so the app's access\n"
           "# rules cover everything; Apache serves nothing from here itself.\n"
           "Options -Indexes +ExecCGI\n"
           "DirectoryIndex disabled\n"
           "# Webhooks may need the Authorization header, which Apache keeps from\n"
           "# CGI programs unless told (Apache 2.4.13 or later).\n"
           "CGIPassAuth On\n" +
           (fastcgi ? "AddHandler fcgid-script .fcgi\n" : "AddHandler cgi-script .cgi\n") +
           "RewriteEngine On\n"
           "RewriteRule ^" + escaped + "$ - [L]\n"
           "RewriteRule ^ " + entry + " [L,E=HTTP_AUTHORIZATION:%{HTTP:Authorization}]\n";
}

int runCgiSetup(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const char* usage =
        "Usage: fourfold cgi-setup <app-folder> <web-root> [--fastcgi] [--binary PATH] [--force]\n";
    std::vector<std::string> positional;
    bool fastcgi = false, force = false;
    std::string binary;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--fastcgi") {
            fastcgi = true;
        } else if (args[i] == "--force") {
            force = true;
        } else if (args[i] == "--binary" && i + 1 < args.size()) {
            binary = args[++i];
        } else if (!args[i].empty() && args[i][0] != '-') {
            positional.push_back(args[i]);
        } else {
            err << usage;
            return 2;
        }
    }
    if (positional.size() != 2) {
        err << usage;
        return 2;
    }

    // Checked before anything is made: the app folder must be out of the
    // web server's reach.
    std::error_code ec;
    std::string appPath = fs::weakly_canonical(fs::absolute(positional[0]), ec).string();
    std::string webPath = fs::weakly_canonical(fs::absolute(positional[1]), ec).string();
    fs::create_directories(webPath, ec);
    std::string webRoot = realPath(webPath);
    if (webRoot.empty()) {
        err << "fourfold cgi-setup: can't open the web root " << positional[1] << "\n";
        return 1;
    }
    std::string problem = webRootProblem(appPath, webRoot);
    if (!problem.empty()) {
        err << "fourfold cgi-setup: " << problem << "\n";
        return 1;
    }
    // The app folder, with its partitions (made if missing).
    fs::create_directories(appPath, ec);
    auto app = AppFolder::open(appPath, AppFolder::Missing::Create);
    if (!app) {
        err << "fourfold cgi-setup: " << app.error().message << "\n";
        return 1;
    }
    std::string appRoot = app.value().root();
    problem = webRootProblem(appRoot, webRoot);
    if (!problem.empty()) {
        err << "fourfold cgi-setup: " << problem << "\n";
        return 1;
    }
    if (binary.empty()) binary = executablePath();
    binary = realPath(binary);
    if (binary.empty() || ::access(binary.c_str(), X_OK) != 0) {
        err << "fourfold cgi-setup: can't find the fourfold program to run; give it with --binary PATH\n";
        return 1;
    }
    std::string binaryFolder = fs::path(binary).parent_path().string();
    if (binaryFolder == webRoot || binaryFolder.rfind(webRoot + "/", 0) == 0) {
        err << "fourfold cgi-setup: the fourfold program is inside the web root; put it outside (e.g. ~/bin/)\n";
        return 1;
    }

    std::string htaccess = webRoot + "/.htaccess";
    std::string entry = webRoot + (fastcgi ? "/fourfold.fcgi" : "/fourfold.cgi");
    std::string other = webRoot + (fastcgi ? "/fourfold.cgi" : "/fourfold.fcgi");
    if (fs::exists(htaccess) && !writtenByUs(htaccess) && !force) {
        err << "fourfold cgi-setup: " << htaccess
            << " exists, and wasn't written by cgi-setup; move it aside (or run again with --force to replace it)\n";
        return 1;
    }
    std::string error;
    if (!writeFile(entry, cgiEntryScript(binary, appRoot, fastcgi), 0755, error) ||
        !writeFile(htaccess, cgiHtaccess(fastcgi), 0644, error)) {
        err << "fourfold cgi-setup: " << error << "\n";
        return 1;
    }
    if (fs::exists(other) && writtenByUs(other)) fs::remove(other, ec);

    out << "Set up " << webRoot << " to serve " << appRoot << " through "
        << (fastcgi ? "FastCGI (mod_fcgid)" : "CGI") << ":\n"
        << "  " << entry << "   runs " << binary << "\n"
        << "  " << htaccess << "   sends every request to it\n";
    // Anything else there is hidden now.
    std::vector<std::string> hidden;
    for (const auto& e : fs::directory_iterator(webRoot, ec)) {
        std::string name = e.path().filename().string();
        if (name != ".htaccess" && name != "fourfold.cgi" && name != "fourfold.fcgi") hidden.push_back(name);
    }
    if (!hidden.empty()) {
        out << "\nThese are in the web root, but won't be served (every request goes to the app):\n";
        for (const std::string& h : hidden) out << "  " << h << "\n";
    }
    out << "\nNext:\n"
        << "  - Upload the app's code/ into " << appRoot << "/code/, then touch code/.deploy to load it at once\n"
        << "    (otherwise it loads once uploads have stopped for a couple of seconds).\n"
        << "  - If the app has jobs or a schedule, run them from cron every minute:\n"
        << "      * * * * * " << shellQuote(binary) << " run-jobs " << shellQuote(appRoot) << "\n"
        << "  - Set config.domain in private/config/app.ms, for sign-in links.\n"
        << "  - Problems are logged in " << appRoot << "/private/state/logs/.\n"
        << "\nCGI mode has no live updates (SSE): pages poll instead, e.g. hx-trigger=\"every 5s\".\n";
    return 0;
}

}  // namespace fourfold::cli
