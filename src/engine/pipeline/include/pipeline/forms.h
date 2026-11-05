// Submitted forms (overview §4, Forms and validation; §8, Connection and
// request limits): reading a request body under limits, checking uploads by
// content, and normalizing and validating the fields against a form
// declaration. Part of the pipeline's Parse stage; plain C++, so each piece
// can be tested on its own.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/datum.h"
#include "compiledapp/compiled_app.h"
#include "http/message.h"
#include "script/script_host.h"

namespace fourfold {

// The limits a body is read under, before any handler sees it.
struct SubmissionLimits {
    size_t maxFormBytes = 1024 * 1024;   // a body's fields (uploads aside)
    size_t maxFields = 1000;             // fields in an url-encoded body
    size_t maxParts = 100;               // parts in a multipart body
    size_t maxPartHeaderBytes = 8 * 1024;
};

// An uploaded file as sent, before it's checked.
struct RawUpload {
    std::string field;
    std::string filename;   // as sent
    std::shared_ptr<const std::string> bytes;
};

// A request body's fields and files, as sent (url-encoded or
// multipart/form-data); the engine's own fields taken out.
struct Submission {
    RequestFacts::Pairs fields;      // decoded, in order; repeats kept
    std::vector<RawUpload> uploads;
    // The engine's fields: the CSRF token, the timing token, and the
    // proof-of-work answer.
    std::optional<std::string> csrf, formTime, challenge;
};

// Why a body couldn't be read: 400 (malformed) or 413 (over a limit).
struct SubmissionError {
    int status = 400;
    std::string message;
};

// Reads a POST body that's url-encoded or multipart/form-data; other bodies
// (and other methods) give an empty submission. Field names and values are
// made valid UTF-8 (bad bytes as U+FFFD), since handlers get them as
// strings.
std::optional<Submission> readSubmission(const HttpRequest& request, const SubmissionLimits& limits,
                                         SubmissionError& error);

// The type a file's content shows it to be: "image/png", "image/jpeg",
// "image/gif", "image/webp", "application/pdf", "application/zip",
// "text/plain" (valid UTF-8 with no NUL or other binary control
// characters), or "application/octet-stream" for anything else.
std::string detectContentType(std::string_view bytes);

// Text as a form field becomes (overview §4): Unicode NFC, control
// characters removed (except tab and newline when `multiline`, with CR LF
// and CR as newlines), and surrounding whitespace trimmed.
std::string normalizeFormText(std::string_view text, bool multiline);

// The URLs in text, as maxLinks counts them: each http://, https:// or
// www. (any case).
size_t countLinks(std::string_view text);

// A form checked against its declaration: what the handler sees.
struct CheckedForm {
    Datum form;                    // req.form: the declared fields, typed
    std::optional<Datum> errors;   // req.errors: by field name; nullopt if none
    std::vector<UploadFacts> files;   // req.files
    Datum typed;                   // what the visitor typed, for reshow()
};

CheckedForm checkForm(const FormSpec& spec, const Submission& submission);

}  // namespace fourfold
