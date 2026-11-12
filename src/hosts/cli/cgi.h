// `fourfold cgi`: one request per process, as a web server's CGI program
// (overview §8, Hosting modes; M10). `fourfold cgi-setup` writes the entry
// point that runs it.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "cgi/cgi_request.h"

namespace fourfold::cli {

// Serves one request, described by `vars` (the CGI environment) and `in`
// (the body), for the app at `folder`, writing the CGI response to `out`
// and problems to `err` (the web server's error log). If `timing` is set,
// how long the request took is written to `err` as well.
int runCgi(const std::string& folder, const CgiVariables& vars, std::istream& in, std::ostream& out,
           std::ostream& err, bool timing);

// `fourfold run-jobs <app-folder> [--max-seconds N] [--server-config FILE]`:
// runs the app's due jobs and scheduled jobs, for CGI mode, where nothing
// runs between requests (overview §2, Background jobs; §8). Meant for cron,
// every minute; it stops once nothing is due, or after --max-seconds (50 by
// default, so runs don't overlap), and prints only problems. If a Fourfold
// server runs the app (it runs jobs itself), or another run-jobs is still
// going, it does nothing.
int runJobs(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

// `fourfold fastcgi <app-folder> [--listen PATH|HOST:PORT]`: serves the app
// through FastCGI, many requests per process, one at a time (overview §8;
// M10). Without --listen, it accepts on the socket a web server's
// FastCGI module (mod_fcgid) gives it as stdin; with it, on a Unix socket
// (a path) or a TCP port on HOST, for mod_proxy_fcgi. The compiled version
// stays in memory, checked against code/ on each request. Stops at SIGTERM
// or SIGINT, after the request in progress.
int runFastCgi(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

// `fourfold cgi-setup <app-folder> <web-root> [--fastcgi] [--binary PATH]
// [--force]`: writes the web root's entry point (fourfold.cgi, or
// fourfold.fcgi for FastCGI under mod_fcgid: a two-line shell script that
// runs this program on the app) and an .htaccess that sends every request
// to it. Refuses an app folder the web server could reach (webRootProblem).
int runCgiSetup(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

// What cgi-setup writes (exposed for tests).
std::string cgiEntryScript(const std::string& binary, const std::string& appRoot, bool fastcgi);
std::string cgiHtaccess(bool fastcgi);

// The CGI environment of this process: every variable.
CgiVariables processEnvironment();

// Why the app at `appRoot` (a real path) mustn't be served from web root
// `webRoot`, or empty if it may: the web server could serve the app's own
// files if the app folder is inside the web root, or the web root is the
// app folder or inside one of its partitions.
std::string webRootProblem(const std::string& appRoot, const std::string& webRoot);

}  // namespace fourfold::cli
