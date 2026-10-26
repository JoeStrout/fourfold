// libFuzzer target: the template parser, then rendering what parses, with
// hostile values. Templates come only from CodeFile, so each input goes
// through a scratch code tree.
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>

#include "partition/app_folder.h"
#include "template/template.h"
#include "temp_dir.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, size_t size) {
    using namespace fourfold;
    static test::TempDir dir;
    static std::unique_ptr<CodeTree> code;
    if (!code) {
        auto tree = AppFolder::open(dir.path(), AppFolder::Missing::Create).value().codeTree();
        if (!tree) std::abort();
        code = std::make_unique<CodeTree>(std::move(tree.value()));
    }
    dir.write("code/frags/f.html", std::string(reinterpret_cast<const char*>(data), size));
    auto file = code->read(RelPath::parse("frags/f.html").value());
    if (!file) return 0;   // too large, say
    for (TemplateKind kind : {TemplateKind::Fragment, TemplateKind::Page}) {
        ParsedTemplate p = parseTemplate(file.value(), kind);
        if (!p.ok()) continue;
        MapFields fields;
        for (const std::string& name : p.compiled.fieldNames()) {
            fields.set(name, FieldValue::string("\"'<>&`/ javascript:x </script>"));
        }
        render(p.compiled, fields);
    }
    return 0;
}
