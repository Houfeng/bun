#include "root.h"
#include "helpers.h"

#include "JavaScriptCore/CustomGetterSetter.h"
#include "JavaScriptCore/Identifier.h"
#include "JavaScriptCore/JSObject.h"
#include "JavaScriptCore/JSArrayBuffer.h"
#include "JavaScriptCore/JSArrayBufferView.h"
#include "JavaScriptCore/JSGenericTypedArrayView.h"
#include "JavaScriptCore/TypedArrayAdaptors.h"

#include <span>
#include <string>
#include <unordered_map>
#include <mutex>

namespace Bun {
using namespace JSC;

using BunEmbedGetterFn = uint64_t (*)(void* ctx, uint64_t this_value);
using BunEmbedSetterFn = void (*)(void* ctx, uint64_t this_value, uint64_t value);
using BunEmbedFinalizerFn = void (*)(void* userdata);

struct AccessorKey {
    uintptr_t object_ptr;
    std::string name;

    bool operator==(const AccessorKey& other) const
    {
        return object_ptr == other.object_ptr && name == other.name;
    }
};

struct AccessorKeyHash {
    size_t operator()(const AccessorKey& key) const
    {
        const size_t h1 = std::hash<uintptr_t> {}(key.object_ptr);
        const size_t h2 = std::hash<std::string> {}(key.name);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

struct AccessorEntry {
    BunEmbedGetterFn getter;
    BunEmbedSetterFn setter;
};

static std::unordered_map<AccessorKey, AccessorEntry, AccessorKeyHash> s_accessor_map;
static std::mutex s_accessor_map_mutex;

static AccessorKey makeKey(EncodedJSValue thisValue, PropertyName propertyName)
{
    JSValue decoded = JSValue::decode(thisValue);
    auto* object = decoded.getObject();

    ZigString zig_name = Zig::toZigString(propertyName.publicName());
    std::string name;
    if (zig_name.ptr && zig_name.len > 0) {
        name.assign(reinterpret_cast<const char*>(zig_name.ptr), zig_name.len);
    }

    return AccessorKey {
        .object_ptr = reinterpret_cast<uintptr_t>(object),
        .name = std::move(name),
    };
}

JSC_DEFINE_CUSTOM_GETTER(BunEmbed_customGetter, (JSGlobalObject * globalObject, EncodedJSValue thisValue, PropertyName propertyName))
{
    AccessorEntry entry { nullptr, nullptr };
    {
        std::lock_guard<std::mutex> lock(s_accessor_map_mutex);
        auto it = s_accessor_map.find(makeKey(thisValue, propertyName));
        if (it == s_accessor_map.end() || !it->second.getter)
            return JSValue::encode(jsUndefined());
        entry = it->second;
    }

    return static_cast<EncodedJSValue>(entry.getter(static_cast<void*>(globalObject), static_cast<uint64_t>(thisValue)));
}

JSC_DEFINE_CUSTOM_SETTER(BunEmbed_customSetter, (JSGlobalObject * globalObject, EncodedJSValue thisValue, EncodedJSValue value, PropertyName propertyName))
{
    AccessorEntry entry { nullptr, nullptr };
    {
        std::lock_guard<std::mutex> lock(s_accessor_map_mutex);
        auto it = s_accessor_map.find(makeKey(thisValue, propertyName));
        if (it == s_accessor_map.end() || !it->second.setter)
            return false;
        entry = it->second;
    }

    entry.setter(static_cast<void*>(globalObject), static_cast<uint64_t>(thisValue), static_cast<uint64_t>(value));
    return true;
}

extern "C" bool BunEmbed__defineCustomAccessor(
    JSGlobalObject* globalObject,
    EncodedJSValue objectValue,
    const char* keyPtr,
    size_t keyLen,
    BunEmbedGetterFn getter,
    BunEmbedSetterFn setter,
    uint32_t flags)
{
    if (!globalObject || !keyPtr || keyLen == 0 || !getter)
        return false;

    JSValue value = JSValue::decode(objectValue);
    JSObject* object = value.getObject();
    if (!object)
        return false;

    {
        std::lock_guard<std::mutex> lock(s_accessor_map_mutex);
        AccessorKey key {
            .object_ptr = reinterpret_cast<uintptr_t>(object),
            .name = std::string(keyPtr, keyLen),
        };
        s_accessor_map[key] = AccessorEntry {
            .getter = getter,
            .setter = setter,
        };
    }

    VM& vm = globalObject->vm();
    WTF::String keyString = WTF::String::fromUTF8(std::span { keyPtr, keyLen });
    if (keyString.isNull())
        return false;

    unsigned attributes = 0;
    if (flags & (1u << 0))
        attributes |= PropertyAttribute::ReadOnly;
    if (flags & (1u << 1))
        attributes |= PropertyAttribute::DontEnum;
    if (flags & (1u << 2))
        attributes |= PropertyAttribute::DontDelete;

    auto* custom = CustomGetterSetter::create(vm, BunEmbed_customGetter, setter ? BunEmbed_customSetter : nullptr);
    object->putDirectCustomAccessor(vm, Identifier::fromString(vm, keyString), custom, attributes);
    return true;
}

extern "C" bool BunEmbed__defineFinalizer(
    JSGlobalObject* globalObject,
    EncodedJSValue objectValue,
    BunEmbedFinalizerFn finalizer,
    void* userdata)
{
    if (!globalObject || !finalizer)
        return false;

    JSValue value = JSValue::decode(objectValue);
    JSObject* object = value.getObject();
    if (!object)
        return false;

    VM& vm = globalObject->vm();
    vm.heap.addFinalizer(object, [finalizer, userdata](JSCell*) -> void {
        finalizer(userdata);
    });
    return true;
}

// ---------------------------------------------------------------------------
// ArrayBuffer / TypedArray creation from external C memory (zero-copy)
// ---------------------------------------------------------------------------

/// BunTypedArrayKind values must stay in sync with the C header and Zig enum.
enum BunTypedArrayKind : uint32_t {
    BunTypedArrayKindInt8 = 0,
    BunTypedArrayKindUint8 = 1,
    BunTypedArrayKindUint8C = 2, // Uint8Clamped
    BunTypedArrayKindInt16 = 3,
    BunTypedArrayKindUint16 = 4,
    BunTypedArrayKindInt32 = 5,
    BunTypedArrayKindUint32 = 6,
    BunTypedArrayKindFloat32 = 7,
    BunTypedArrayKindFloat64 = 8,
    BunTypedArrayKindBigInt64 = 9,
    BunTypedArrayKindBigUint64 = 10,
};

/// Create a JS ArrayBuffer that directly references external C memory.
/// finalizer(userdata) is called by the GC when the buffer is collected.
/// Returns zero (exception sentinel) on failure.
extern "C" JSC::EncodedJSValue BunEmbed__createArrayBuffer(
    JSGlobalObject* globalObject,
    void* data,
    size_t byteLength,
    BunEmbedFinalizerFn finalizer,
    void* userdata)
{
    if (!globalObject)
        return {};

    VM& vm = globalObject->vm();

    auto destructor = [finalizer, userdata](void*) {
        if (finalizer)
            finalizer(userdata);
    };

    auto arrayBuffer = ArrayBuffer::createFromBytes(
        { reinterpret_cast<const uint8_t*>(data), byteLength },
        createSharedTask<void(void*)>(WTF::move(destructor)));

    auto* buffer = JSC::JSArrayBuffer::create(
        vm,
        globalObject->arrayBufferStructure(ArrayBufferSharingMode::Default),
        WTF::move(arrayBuffer));

    if (!buffer) [[unlikely]]
        return {};

    return JSValue::encode(buffer);
}

/// Create a JS TypedArray view over external C memory.
/// finalizer(userdata) is called by the GC when the underlying ArrayBuffer is collected.
/// Returns zero (exception sentinel) on failure.
extern "C" JSC::EncodedJSValue BunEmbed__createTypedArray(
    JSGlobalObject* globalObject,
    uint32_t kind,
    void* data,
    size_t elementCount,
    BunEmbedFinalizerFn finalizer,
    void* userdata)
{
    if (!globalObject)
        return {};

    auto destructor = [finalizer, userdata](void*) {
        if (finalizer)
            finalizer(userdata);
    };

    // Compute byte length based on element size for the given kind.
    size_t elementSize;
    switch (static_cast<BunTypedArrayKind>(kind)) {
    case BunTypedArrayKindInt8:
    case BunTypedArrayKindUint8:
    case BunTypedArrayKindUint8C:
        elementSize = 1;
        break;
    case BunTypedArrayKindInt16:
    case BunTypedArrayKindUint16:
        elementSize = 2;
        break;
    case BunTypedArrayKindInt32:
    case BunTypedArrayKindUint32:
    case BunTypedArrayKindFloat32:
        elementSize = 4;
        break;
    case BunTypedArrayKindFloat64:
    case BunTypedArrayKindBigInt64:
    case BunTypedArrayKindBigUint64:
        elementSize = 8;
        break;
    default:
        return {};
    }

    // Guard against size_t overflow before computing the byte length.
    if (elementCount > 0 && elementSize > 0 && elementCount > (SIZE_MAX / elementSize)) {
        // Call finalizer immediately so the caller's memory is not orphaned.
        if (finalizer)
            finalizer(userdata);
        return {};
    }
    size_t byteLength = elementCount * elementSize;

    auto arrayBuffer = ArrayBuffer::createFromBytes(
        { reinterpret_cast<const uint8_t*>(data), byteLength },
        createSharedTask<void(void*)>(WTF::move(destructor)));

    JSC::JSArrayBufferView* view = nullptr;

    switch (static_cast<BunTypedArrayKind>(kind)) {
    case BunTypedArrayKindInt8:
        view = JSC::JSInt8Array::create(globalObject,
            globalObject->typedArrayStructure(TypeInt8, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindUint8:
        view = JSC::JSUint8Array::create(globalObject,
            globalObject->typedArrayStructure(TypeUint8, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindUint8C:
        view = JSC::JSUint8ClampedArray::create(globalObject,
            globalObject->typedArrayStructure(TypeUint8Clamped, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindInt16:
        view = JSC::JSInt16Array::create(globalObject,
            globalObject->typedArrayStructure(TypeInt16, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindUint16:
        view = JSC::JSUint16Array::create(globalObject,
            globalObject->typedArrayStructure(TypeUint16, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindInt32:
        view = JSC::JSInt32Array::create(globalObject,
            globalObject->typedArrayStructure(TypeInt32, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindUint32:
        view = JSC::JSUint32Array::create(globalObject,
            globalObject->typedArrayStructure(TypeUint32, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindFloat32:
        view = JSC::JSFloat32Array::create(globalObject,
            globalObject->typedArrayStructure(TypeFloat32, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindFloat64:
        view = JSC::JSFloat64Array::create(globalObject,
            globalObject->typedArrayStructure(TypeFloat64, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindBigInt64:
        view = JSC::JSBigInt64Array::create(globalObject,
            globalObject->typedArrayStructure(TypeBigInt64, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    case BunTypedArrayKindBigUint64:
        view = JSC::JSBigUint64Array::create(globalObject,
            globalObject->typedArrayStructure(TypeBigUint64, false),
            WTF::move(arrayBuffer), 0, elementCount);
        break;
    default:
        return {};
    }

    if (!view) [[unlikely]]
        return {};

    return JSValue::encode(view);
}

}
