// =============================================================================
// src/vm/values/value.cc
// =============================================================================

#include "vm/values/value.h"

#include "vm/objects/object.h"

namespace v12 {

const char* HeapObjectKindName(HeapObjectKind kind) {
    switch (kind) {
        case HeapObjectKind::kObject:        return "Object";
        case HeapObjectKind::kArray:         return "Array";
        case HeapObjectKind::kString:        return "String";
        case HeapObjectKind::kFunction:      return "Function";
        case HeapObjectKind::kBoundFunction: return "BoundFunction";
        case HeapObjectKind::kNumber:        return "Number";
        case HeapObjectKind::kBoolean:       return "Boolean";
        case HeapObjectKind::kUndefined:     return "Undefined";
        case HeapObjectKind::kNull:          return "Null";
        case HeapObjectKind::kSymbol:        return "Symbol";
        case HeapObjectKind::kMap:           return "Map";
        case HeapObjectKind::kSet:           return "Set";
        case HeapObjectKind::kPromise:       return "Promise";
        case HeapObjectKind::kError:         return "Error";
        case HeapObjectKind::kArrayBuffer:   return "ArrayBuffer";
        case HeapObjectKind::kTypedArray:    return "TypedArray";
        case HeapObjectKind::kWeakRef:       return "WeakRef";
        case HeapObjectKind::kExternal:      return "External";
    }
    return "<unknown>";
}

HeapObjectKind HeapObject::kind() const {
    return shape()->object_kind();
}

Shape* HeapObject::shape() const {
    // Mask off the low 2 mark bits.
    return reinterpret_cast<Shape*>(shape_and_mark_ & ~uintptr_t{3});
}

void HeapObject::set_shape(Shape* s) {
    uintptr_t mark = shape_and_mark_ & 3;
    shape_and_mark_ = reinterpret_cast<uintptr_t>(s) | mark;
}

JSObject* HeapObject::AsObject() {
    V12_DCHECK(kind() == HeapObjectKind::kObject, "AsObject on non-object");
    return static_cast<JSObject*>(this);
}
JSArray* HeapObject::AsArray() {
    V12_DCHECK(kind() == HeapObjectKind::kArray, "AsArray on non-array");
    return static_cast<JSArray*>(this);
}
JSString* HeapObject::AsString() {
    V12_DCHECK(kind() == HeapObjectKind::kString, "AsString on non-string");
    return static_cast<JSString*>(this);
}
JSFunction* HeapObject::AsFunction() {
    V12_DCHECK(kind() == HeapObjectKind::kFunction, "AsFunction on non-function");
    return static_cast<JSFunction*>(this);
}

bool Value::IsNumber() const {
    if (IsSmi()) return true;
    return IsHeapObject() && AsHeapObject()->kind() == HeapObjectKind::kNumber;
}
bool Value::IsString() const {
    return IsHeapObject() && AsHeapObject()->kind() == HeapObjectKind::kString;
}
bool Value::IsObject() const {
    return IsHeapObject() && AsHeapObject()->kind() == HeapObjectKind::kObject;
}
bool Value::IsArray() const {
    return IsHeapObject() && AsHeapObject()->kind() == HeapObjectKind::kArray;
}
bool Value::IsFunction() const {
    if (!IsHeapObject()) return false;
    HeapObjectKind k = AsHeapObject()->kind();
    return k == HeapObjectKind::kFunction || k == HeapObjectKind::kExternal;
}
bool Value::IsHostFunction() const {
    return IsHeapObject() && AsHeapObject()->kind() == HeapObjectKind::kExternal;
}
bool Value::IsBoolean() const {
    if (IsHeapObject()) {
        return AsHeapObject()->kind() == HeapObjectKind::kBoolean;
    }
    return false;
}
bool Value::IsUndefined() const {
    return IsHeapObject() && AsHeapObject()->kind() == HeapObjectKind::kUndefined;
}
bool Value::IsNull() const {
    return IsHeapObject() && AsHeapObject()->kind() == HeapObjectKind::kNull;
}

double Value::AsNumber() const {
    if (IsSmi()) return static_cast<double>(AsSmi());
    V12_DCHECK(IsNumber(), "AsNumber on non-number");
    return static_cast<JSNumber*>(AsHeapObject())->value();
}

JSString* Value::AsString() const {
    V12_DCHECK(IsString(), "AsString on non-string");
    return static_cast<JSString*>(AsHeapObject());
}
JSObject* Value::AsObject() const {
    V12_DCHECK(IsObject(), "AsObject on non-object");
    return static_cast<JSObject*>(AsHeapObject());
}
JSArray* Value::AsArray() const {
    V12_DCHECK(IsArray(), "AsArray on non-array");
    return static_cast<JSArray*>(AsHeapObject());
}
JSFunction* Value::AsFunction() const {
    V12_DCHECK(IsFunction(), "AsFunction on non-function");
    return static_cast<JSFunction*>(AsHeapObject());
}
HostFunction* Value::AsHostFunction() const {
    V12_DCHECK(IsHostFunction(), "AsHostFunction on non-host-function");
    return static_cast<HostFunction*>(AsHeapObject());
}

}  // namespace v12
