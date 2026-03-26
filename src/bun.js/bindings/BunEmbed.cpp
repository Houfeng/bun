#include "root.h"
#include "helpers.h"

#include "JavaScriptCore/CustomGetterSetter.h"
#include "JavaScriptCore/Identifier.h"
#include "JavaScriptCore/JSObject.h"

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
        const size_t h1 = std::hash<uintptr_t>{}(key.object_ptr);
        const size_t h2 = std::hash<std::string>{}(key.name);
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

}
