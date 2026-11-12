#include "cgi/schema_lock.h"

namespace fourfold {

Result<std::optional<SchemaFileLock::Shared>> SchemaFileLock::enter(std::chrono::milliseconds wait) const {
    auto deadline = std::chrono::steady_clock::now() + wait;
    auto gateFile = state_.lockFile("schema-gate");
    if (!gateFile) return gateFile.error();
    auto gate = FileLock::acquireWithin(std::move(gateFile).value(), FileLock::Mode::Shared, wait);
    if (!gate) return gate.error();
    if (!gate.value()) return std::optional<Shared>();
    auto schemaFile = state_.lockFile("schema");
    if (!schemaFile) return schemaFile.error();
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    // With the gate held, only a migration finishing up can hold "schema".
    auto schema = FileLock::acquireWithin(std::move(schemaFile).value(), FileLock::Mode::Shared,
                                          std::max(left, std::chrono::milliseconds(0)));
    if (!schema) return schema.error();
    if (!schema.value()) return std::optional<Shared>();
    return std::optional<Shared>(Shared{std::move(*schema.value())});
}

Result<SchemaFileLock::Exclusive> SchemaFileLock::exclusive() const {
    auto gateFile = state_.lockFile("schema-gate");
    if (!gateFile) return gateFile.error();
    auto gate = FileLock::acquire(std::move(gateFile).value());
    if (!gate) return gate.error();
    auto schemaFile = state_.lockFile("schema");
    if (!schemaFile) return schemaFile.error();
    auto schema = FileLock::acquire(std::move(schemaFile).value());
    if (!schema) return schema.error();
    return Exclusive{std::move(gate).value(), std::move(schema).value()};
}

void FileSchemaGate::enter(Admitted admitted) {
    bool ok = true;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (inside_ == 0) {
            auto shared = lock_.enter(wait_);
            ok = shared && shared.value();
            if (ok) held_ = std::move(*shared.value());
        }
        if (ok) inside_++;
    }
    admitted(ok);
}

void FileSchemaGate::leave() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (inside_ > 0 && --inside_ == 0) held_.reset();
}

}  // namespace fourfold
