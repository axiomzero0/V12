// =============================================================================
// src/vm/isolate/isolate.cc
// =============================================================================

#include "vm/isolate/isolate.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "gc/heap.h"
#include "vm/objects/object.h"
#include "vm/shapes/shape.h"

namespace v12 {

namespace {

Isolate* g_current_isolate = nullptr;

// Simple string interner backed by an unordered_map<string, string_view>.
class InternerImpl : public Interner {
public:
    std::string_view Intern(std::string_view s) {
        auto it = strings_.find(std::string(s));
        if (it != strings_.end()) return it->second;
        // Allocate a permanent copy.
        char* buf = new char[s.size() + 1];
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
        std::string_view stored(buf, s.size());
        strings_[std::string(s)] = stored;
        return stored;
    }
    bool IsInterned(std::string_view s) const {
        return strings_.find(std::string(s)) != strings_.end();
    }
private:
    std::unordered_map<std::string, std::string_view> strings_;
};

}  // namespace

Isolate::Isolate() {
    g_current_isolate = this;
    heap_ = std::make_unique<Heap>(this);
    interner_ = std::make_unique<InternerImpl>();
    InitializeRoots();
    InitializeShapes();
    InitializeGlobals();
}

Isolate::~Isolate() {
    if (g_current_isolate == this) {
        g_current_isolate = nullptr;
    }
}

Isolate* Isolate::Current() {
    V12_CHECK(g_current_isolate != nullptr, "no current isolate");
    return g_current_isolate;
}

void Isolate::InitializeRoots() {
    // Singletons for undefined/null/true/false.
    // For now these are heap-allocated; in a real engine they'd be special
    // read-only objects.
    // We'll defer to JSUndefined/JSNull/JSBoolean which need Shape setup.
}

void Isolate::InitializeShapes() {
    empty_shape_ = Shape::Empty(this);
    array_shape_ = Shape::ArrayShape(this);
    function_shape_ = Shape::FunctionShape(this);
}

void Isolate::InitializeGlobals() {
    // global_object_ = JSObject::New(this, empty_shape_);
    // TODO: once JSObject::New is implemented.
}

std::string_view Isolate::Intern(std::string_view s) {
    return interner_->Intern(s);
}

bool Isolate::IsInterned(std::string_view s) const {
    return interner_->IsInterned(s);
}

void* Isolate::Allocate(uint32_t size) {
    void* p = heap_->Allocate(size);
    stats_.bytes_allocated += size;
    stats_.allocations += 1;
    return p;
}

// Interner base class destructor.
Interner::~Interner() = default;

}  // namespace v12
