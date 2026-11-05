// The Parse stage's forms (overview §4, Forms and validation): reading
// bodies under limits, checking uploads by content, normalizing text, and
// checking fields against a declaration, tampered submissions included.
#include "pipeline/forms.h"
#include "pipeline/stages.h"
#include "platform/address.h"

#include "doctest.h"

using namespace fourfold;

namespace {

HttpRequest post(const std::string& contentType, std::string body) {
    HttpRequest r;
    r.method = "POST";
    r.target = "/x";
    r.headers.add("Content-Type", contentType);
    r.body = std::move(body);
    return r;
}

const char* const kPng = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR";
std::string png() { return std::string(kPng, 16) + "rest of the image"; }

std::string multipart(const std::vector<std::tuple<std::string, std::string, std::string>>& parts,
                      const std::string& boundary = "XyZ") {
    std::string body;
    for (const auto& [name, filename, content] : parts) {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"";
        if (!filename.empty()) body += "; filename=\"" + filename + "\"\r\nContent-Type: image/png";
        body += "\r\n\r\n" + content + "\r\n";
    }
    return body + "--" + boundary + "--\r\n";
}

FormField field(const char* name, FormFieldType type) {
    FormField f;
    f.name = name;
    f.type = type;
    return f;
}

Submission fields(RequestFacts::Pairs pairs) {
    Submission s;
    s.fields = std::move(pairs);
    return s;
}

const Datum& at(const Datum& map, const std::string& key) {
    static const Datum none;
    auto it = map.asMap().find(key);
    return it == map.asMap().end() ? none : it->second;
}

std::string errorOf(const CheckedForm& c, const std::string& name) {
    if (!c.errors) return "";
    const Datum& e = at(*c.errors, name);
    return e.kind() == Datum::Kind::String ? e.asString() : "";
}

}  // namespace

TEST_CASE("readSubmission: url-encoded, with the engine's fields taken out") {
    SubmissionError error;
    auto s = readSubmission(post("application/x-www-form-urlencoded; charset=UTF-8",
                                 "who=Ada&_csrf=C&_t=T&_pow=P&who=Again&x=%FF"),
                            {}, error);
    REQUIRE(s);
    REQUIRE_EQ(s->fields.size(), 3u);
    CHECK_EQ(s->fields[0].second, "Ada");
    CHECK_EQ(s->fields[1].second, "Again");
    CHECK_EQ(s->fields[2].second, "\xEF\xBF\xBD");   // invalid UTF-8, replaced
    CHECK_EQ(s->csrf.value_or(""), "C");
    CHECK_EQ(s->formTime.value_or(""), "T");
    CHECK_EQ(s->challenge.value_or(""), "P");

    // Other methods and bodies: nothing.
    HttpRequest get = post("application/x-www-form-urlencoded", "a=1");
    get.method = "GET";
    CHECK(readSubmission(get, {}, error)->fields.empty());
    CHECK(readSubmission(post("application/json", "{\"a\": 1}"), {}, error)->fields.empty());
}

TEST_CASE("readSubmission: multipart, with files kept apart") {
    SubmissionError error;
    auto s = readSubmission(
        post("multipart/form-data; boundary=XyZ",
             multipart({{"who", "", "Ada"}, {"_csrf", "", "C"}, {"photo", "../../me.png", png()}, {"note", "", "a\r\nb"}})),
        {}, error);
    REQUIRE(s);
    REQUIRE_EQ(s->fields.size(), 2u);
    CHECK_EQ(s->fields[0], std::make_pair(std::string("who"), std::string("Ada")));
    CHECK_EQ(s->fields[1].second, "a\r\nb");
    CHECK_EQ(s->csrf.value_or(""), "C");
    REQUIRE_EQ(s->uploads.size(), 1u);
    CHECK_EQ(s->uploads[0].field, "photo");
    CHECK_EQ(s->uploads[0].filename, "../../me.png");   // cleaned later, by checkForm
    CHECK_EQ(*s->uploads[0].bytes, png());

    // A quoted boundary.
    auto q = readSubmission(post("multipart/form-data; boundary=\"a b\"", multipart({{"k", "", "v"}}, "a b")), {}, error);
    REQUIRE(q);
    CHECK_EQ(q->fields.size(), 1u);
}

TEST_CASE("readSubmission: malformed and oversized bodies are refused") {
    SubmissionError error;
    struct Case {
        const char* name;
        HttpRequest request;
        int status;
    };
    SubmissionLimits limits;
    limits.maxFields = 3;
    limits.maxParts = 2;
    limits.maxFormBytes = 100;
    std::string many = multipart({{"a", "", "1"}, {"b", "", "2"}, {"c", "", "3"}});
    const Case cases[] = {
        {"no boundary", post("multipart/form-data", "x"), 400},
        {"no parts", post("multipart/form-data; boundary=Q", "nothing here"), 400},
        {"cut off", post("multipart/form-data; boundary=XyZ", "--XyZ\r\nContent-Disposition: form-data; name=a\r\n\r\nab"), 400},
        {"no headers end", post("multipart/form-data; boundary=XyZ", "--XyZ\r\nContent-Disposition: x"), 400},
        {"too many parts", post("multipart/form-data; boundary=XyZ", many), 413},
        {"too many fields", post("application/x-www-form-urlencoded", "a=1&b=2&c=3&d=4"), 413},
        {"too large", post("application/x-www-form-urlencoded", "a=" + std::string(200, 'x')), 413},
        {"field too large", post("multipart/form-data; boundary=XyZ", multipart({{"a", "", std::string(200, 'x')}})), 413},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        CHECK_FALSE(readSubmission(c.request, limits, error));
        CHECK_EQ(error.status, c.status);
    }
    // A big file is fine here: uploads are held to their fields' maxKB later.
    CHECK(readSubmission(post("multipart/form-data; boundary=XyZ", multipart({{"f", "a.png", std::string(500, 'x')}})),
                         limits, error));
}

TEST_CASE("detectContentType: by content, not by name or claim") {
    CHECK_EQ(detectContentType(png()), "image/png");
    CHECK_EQ(detectContentType("\xFF\xD8\xFF\xE0 jfif"), "image/jpeg");
    CHECK_EQ(detectContentType("GIF89a..."), "image/gif");
    CHECK_EQ(detectContentType(std::string("RIFF\x10\0\0\0WEBPVP8 ", 16)), "image/webp");
    CHECK_EQ(detectContentType("%PDF-1.7"), "application/pdf");
    CHECK_EQ(detectContentType(std::string("PK\x03\x04", 4) + "zip"), "application/zip");
    CHECK_EQ(detectContentType("plain text\nwith lines\ttabs, and caf\xC3\xA9"), "text/plain");
    CHECK_EQ(detectContentType(std::string("text\0nul", 8)), "application/octet-stream");
    CHECK_EQ(detectContentType("bad \xFF utf-8"), "application/octet-stream");
    CHECK_EQ(detectContentType("<svg onload=alert(1)>"), "text/plain");   // text, and never served as an image
    CHECK_EQ(detectContentType("\x89PNX fake"), "application/octet-stream");
}

TEST_CASE("normalizeFormText: NFC, control characters, whitespace") {
    CHECK_EQ(normalizeFormText("  Ada  ", false), "Ada");
    CHECK_EQ(normalizeFormText("cafe\xCC\x81", false), "caf\xC3\xA9");
    CHECK_EQ(normalizeFormText("a\x01" "b\x7F" "c\xC2\x85" "d", false), "abcd");   // C0, DEL, C1 (NEL)
    CHECK_EQ(normalizeFormText("one\ntwo\tthree", false), "one two three");
    CHECK_EQ(normalizeFormText("one\r\ntwo\rthree\n\tfour", true), "one\ntwo\nthree\n\tfour");
    CHECK_EQ(normalizeFormText("\xC2\xA0 x \xC2\xA0", false), "x");   // no-break spaces trimmed
    CHECK_EQ(normalizeFormText(" \n\t ", true), "");
}

TEST_CASE("countLinks") {
    CHECK_EQ(countLinks("no links"), 0u);
    CHECK_EQ(countLinks("see http://a.example and HTTPS://b.example"), 2u);
    CHECK_EQ(countLinks("www.example.com and https://www.example.com"), 2u);
}

TEST_CASE("checkForm: types, conversion and rules") {
    FormSpec spec;
    spec.source = RelPath::parse("pages/f.ms").value();
    spec.name = "f";
    FormField who = field("who", FormFieldType::Text);
    who.required = true;
    who.max = 5;
    who.messages["required"] = "Sign, please";
    FormField msg = field("msg", FormFieldType::Multiline);
    msg.maxLinks = 1;
    FormField age = field("age", FormFieldType::Int);
    age.min = 0;
    age.max = 150;
    FormField price = field("price", FormFieldType::Number);
    FormField email = field("email", FormFieldType::Email);
    FormField site = field("site", FormFieldType::Url);
    FormField agree = field("agree", FormFieldType::Bool);
    agree.required = true;
    FormField mood = field("mood", FormFieldType::Choice);
    mood.options = {"happy", "meh"};
    FormField tags = field("tags", FormFieldType::Choices);
    tags.options = {"a", "b", "c"};
    FormField day = field("day", FormFieldType::Date);
    spec.fields = {who, msg, age, price, email, site, agree, mood, tags, day};

    SUBCASE("all good") {
        CheckedForm c = checkForm(spec, fields({{"who", " Ada "}, {"msg", "hi\r\nthere"}, {"age", "36"},
                                                 {"price", "-1.5e2"}, {"email", "ada@example.com"},
                                                 {"site", "https://example.com/x"}, {"agree", "on"},
                                                 {"mood", "meh"}, {"tags", "c"}, {"tags", "a"}, {"tags", "a"},
                                                 {"day", "2024-02-29"}}));
        CHECK_FALSE(c.errors);
        CHECK_EQ(at(c.form, "who").asString(), "Ada");
        CHECK_EQ(at(c.form, "msg").asString(), "hi\nthere");
        CHECK_EQ(at(c.form, "age").asNumber(), 36);
        CHECK_EQ(at(c.form, "price").asNumber(), -150);
        CHECK_EQ(at(c.form, "agree").asNumber(), 1);
        CHECK_EQ(at(c.form, "mood").asString(), "meh");
        CHECK_EQ(toJson(at(c.form, "tags")), "[\"a\",\"c\"]");   // in the options' order, once each
        CHECK_EQ(at(c.form, "day").asString(), "2024-02-29");
    }
    SUBCASE("missing: required ones are errors; optional ones are empty") {
        CheckedForm c = checkForm(spec, fields({{"who", "   "}}));
        REQUIRE(c.errors);
        CHECK_EQ(errorOf(c, "who"), "Sign, please");
        CHECK_EQ(errorOf(c, "agree"), "Please check this box.");
        CHECK_EQ(c.errors->asMap().size(), 2u);
        CHECK_EQ(at(c.form, "msg").asString(), "");
        CHECK(at(c.form, "age").isNull());
        CHECK_EQ(at(c.form, "agree").asNumber(), 0);
        CHECK(at(c.form, "mood").isNull());
        CHECK_EQ(toJson(at(c.form, "tags")), "[]");
    }
    SUBCASE("each rule") {
        CheckedForm c = checkForm(spec, fields({{"who", "Ada\nLovelace"}, {"msg", "http://a http://b"},
                                                 {"age", "1e2"}, {"price", "Infinity"}, {"email", "ada at example"},
                                                 {"site", "javascript:alert(1)"}, {"agree", ""}, {"day", "2023-02-29"}}));
        CHECK_EQ(errorOf(c, "who"), "Please keep this to one line.");
        CHECK_EQ(errorOf(c, "msg"), "Please include no more than 1 link.");
        CHECK_EQ(errorOf(c, "age"), "Please enter a whole number.");
        CHECK_EQ(errorOf(c, "price"), "Please enter a number.");
        CHECK_EQ(errorOf(c, "email"), "Please enter an email address.");
        CHECK_EQ(errorOf(c, "site"), "Please enter a web address starting with http:// or https://.");
        CHECK_EQ(errorOf(c, "day"), "Please enter a date (YYYY-MM-DD).");
        CHECK(errorOf(c, "agree").empty());   // present, even empty, means checked
        // Failed fields are null, so the handler can't use them by mistake;
        // what was typed is kept for reshow().
        CHECK(at(c.form, "who").isNull());
        CHECK_EQ(at(c.typed, "email").asString(), "ada at example");
    }
    SUBCASE("lengths count characters; numbers their value") {
        CHECK_FALSE(checkForm(spec, fields({{"who", "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"}, {"agree", "1"}})).errors);
        CHECK_EQ(errorOf(checkForm(spec, fields({{"who", "Adelaide"}, {"agree", "1"}})), "who"),
                 "Please use at most 5 characters.");
        CHECK_EQ(errorOf(checkForm(spec, fields({{"who", "A"}, {"age", "151"}, {"agree", "1"}})), "age"),
                 "Please enter 150 or less.");
        CHECK_EQ(errorOf(checkForm(spec, fields({{"who", "A"}, {"price", "1e400"}, {"agree", "1"}})), "price"),
                 "Please enter a number.");
    }
}

TEST_CASE("checkForm: tampered submissions") {
    FormSpec spec;
    spec.name = "t";
    FormField who = field("who", FormFieldType::Text);
    FormField role = field("mood", FormFieldType::Choice);
    role.options = {"happy", "meh"};
    role.required = true;
    FormField picks = field("picks", FormFieldType::Choices);
    picks.options = {"a", "b"};
    spec.fields = {who, role, picks};

    CheckedForm c = checkForm(spec, fields({{"who", "Eve"}, {"role", "admin"}, {"price", "0"}, {"isAdmin", "1"},
                                             {"mood", "evil"}, {"picks", "a"}, {"picks", "z"}}));
    // Undeclared fields never reach the handler.
    CHECK_EQ(c.form.asMap().size(), 3u);
    CHECK(c.form.asMap().count("role") == 0);
    CHECK(c.form.asMap().count("price") == 0);
    // A choice not in the options is dropped, like an undeclared field.
    CHECK(at(c.form, "mood").isNull());
    CHECK_EQ(errorOf(c, "mood"), "Please choose one.");
    CHECK_EQ(toJson(at(c.form, "picks")), "[\"a\"]");
    // A repeated text field: the first wins.
    CHECK_EQ(at(checkForm(spec, fields({{"who", "one"}, {"who", "two"}, {"mood", "meh"}})).form, "who").asString(), "one");
}

TEST_CASE("checkForm: uploads, checked by content") {
    FormSpec spec;
    spec.name = "up";
    FormField photo = field("photo", FormFieldType::File);
    photo.maxKB = 1;
    photo.types = {"image/png", "image/jpeg"};
    photo.required = true;
    FormField any = field("any", FormFieldType::File);
    any.maxKB = 1;
    any.types = {"image/*"};
    spec.fields = {photo, any};
    auto with = [](std::vector<RawUpload> uploads) {
        Submission s;
        s.uploads = std::move(uploads);
        return s;
    };
    auto bytes = [](std::string b) { return std::make_shared<const std::string>(std::move(b)); };

    CheckedForm ok = checkForm(spec, with({{"photo", "C:\\Users\\me\\cat\x01.png", bytes(png())},
                                           {"any", "a.gif", bytes("GIF89a...")}}));
    CHECK_FALSE(ok.errors);
    REQUIRE_EQ(ok.files.size(), 2u);
    CHECK_EQ(ok.files[0].field, "photo");
    CHECK_EQ(ok.files[0].filename, "cat.png");   // no folders, no control characters
    CHECK_EQ(ok.files[0].contentType, "image/png");
    CHECK_EQ(ok.files[1].contentType, "image/gif");
    CHECK(ok.form.asMap().empty());   // files go in req.files, not req.form

    // A "PNG" that isn't one: refused, whatever its name and claimed type.
    CheckedForm forged = checkForm(spec, with({{"photo", "evil.png", bytes("<script>alert(1)</script>")}}));
    CHECK_EQ(errorOf(forged, "photo"), "Please choose a file of the right type (PNG or JPEG).");
    CHECK(forged.files.empty());
    CHECK_EQ(errorOf(checkForm(spec, with({{"photo", "big.png", bytes(png() + std::string(2000, 'x'))}})), "photo"),
             "Please choose a file no bigger than 1 KB.");
    CHECK_EQ(errorOf(checkForm(spec, with({{"photo", "", bytes("")}})), "photo"), "Please choose a file.");
    CHECK_EQ(errorOf(checkForm(spec, with({{"photo", "a.png", bytes(png())}, {"any", "x.pdf", bytes("%PDF-1.4")}})), "any"),
             "Please choose a file of the right type (an image).");
    // An upload under an undeclared name is dropped.
    CheckedForm extra = checkForm(spec, with({{"photo", "a.png", bytes(png())}, {"other", "b.png", bytes(png())}}));
    CHECK_EQ(extra.files.size(), 1u);
}

TEST_CASE("clientAddress: X-Forwarded-For only from a trusted proxy") {
    std::vector<AddressRange> trust = {*AddressRange::parse("10.0.0.0/8"), *AddressRange::parse("::1")};
    auto from = [](std::string peer, std::vector<std::string> forwarded) {
        HttpRequest r;
        r.peerAddress = std::move(peer);
        for (std::string& f : forwarded) r.headers.add("X-Forwarded-For", std::move(f));
        return r;
    };
    // Not from a proxy: the header is ignored, whatever it says.
    CHECK_EQ(clientAddress(from("203.0.113.9", {"1.2.3.4"}), trust), "203.0.113.9");
    CHECK_EQ(clientAddress(from("203.0.113.9", {"1.2.3.4"}), {}), "203.0.113.9");
    // From the proxy: the address it saw (the rightmost).
    CHECK_EQ(clientAddress(from("10.0.0.2", {"198.51.100.7"}), trust), "198.51.100.7");
    // A visitor's own forged entry, on the left, is skipped.
    CHECK_EQ(clientAddress(from("10.0.0.2", {"6.6.6.6, 198.51.100.7"}), trust), "198.51.100.7");
    // Through two trusted proxies.
    CHECK_EQ(clientAddress(from("::1", {"198.51.100.7, 10.1.2.3"}), trust), "198.51.100.7");
    CHECK_EQ(clientAddress(from("10.0.0.2", {"198.51.100.7", "10.9.9.9"}), trust), "198.51.100.7");
    // Garbage stops the walk at the last address the proxies vouch for.
    CHECK_EQ(clientAddress(from("10.0.0.2", {"not-an-ip"}), trust), "10.0.0.2");
    CHECK_EQ(clientAddress(from("10.0.0.2", {}), trust), "10.0.0.2");
    CHECK_EQ(clientAddress(from("10.0.0.2", {"2001:db8::5"}), trust), "2001:db8::5");
}
