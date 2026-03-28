#include "root.h"
#include "helpers.h"

#include "JavaScriptCore/CustomGetterSetter.h"
#include "JavaScriptCore/Identifier.h"
#include "JavaScriptCore/JSObject.h"
#include "JavaScriptCore/JSFunction.h"
#include "JavaScriptCore/ObjectConstructor.h"
#include "JavaScriptCore/JSArrayBuffer.h"
#include "JavaScriptCore/JSArrayBufferView.h"
#include "JavaScriptCore/JSGenericTypedArrayView.h"
#include "JavaScriptCore/TypedArrayAdaptors.h"

#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace Bun {
using namespace JSC;

using BunEmbedGetterFn = uint64_t (*)(void* ctx, uint64_t this_value);
using BunEmbedSetterFn = void (*)(void* ctx, uint64_t this_value, uint64_t value);
using BunEmbedFinalizerFn = void (*)(void* userdata);
using BunEmbedClassMethodFn = uint64_t (*)(void* ctx, uint64_t this_value, void* native_ptr, int argc, const uint64_t* argv, void* userdata);
using BunEmbedClassGetterFn = uint64_t (*)(void* ctx, uint64_t this_value, void* native_ptr, void* userdata);
using BunEmbedClassSetterFn = void (*)(void* ctx, uint64_t this_value, void* native_ptr, uint64_t value, void* userdata);
using BunEmbedClassFinalizerFn = void (*)(void* native_ptr, void* userdata);

struct BunEmbedClassMethodDescriptor {
    const char* name;
    size_t name_len;
    BunEmbedClassMethodFn callback;
    void* userdata;
    int arg_count;
    int dont_enum;
    int dont_delete;
};

struct BunEmbedClassPropertyDescriptor {
    const char* name;
    size_t name_len;
    BunEmbedClassGetterFn getter;
    BunEmbedClassSetterFn setter;
    void* userdata;
    int read_only;
    int dont_enum;
    int dont_delete;
};

struct BunEmbedClassDescriptor {
    const char* name;
    size_t name_len;
    const BunEmbedClassPropertyDescriptor* properties;
    size_t property_count;
    const BunEmbedClassMethodDescriptor* methods;
    size_t method_count;
};

struct BunEmbedArrayBufferInfo {
    void* data;
    size_t byte_length;
};

struct BunEmbedTypedArrayInfo {
    void* data;
    size_t byte_offset;
    size_t byte_length;
    size_t element_count;
    uint32_t kind;
};

struct BunEmbedRegisteredClass;

struct BunEmbedRegisteredMethod {
    std::string name;
    BunEmbedClassMethodFn callback { nullptr };
    void* userdata { nullptr };
    unsigned argCount { 0 };
    unsigned attributes { 0 };
    JSObject* functionObject { nullptr };
};

struct BunEmbedRegisteredProperty {
    std::string name;
    BunEmbedClassGetterFn getter { nullptr };
    BunEmbedClassSetterFn setter { nullptr };
    void* userdata { nullptr };
    unsigned attributes { 0 };
};

struct BunEmbedRegisteredClass {
    VM* vm { nullptr };
    std::string name;
    BunEmbedRegisteredClass* parent { nullptr };
    JSObject* prototype { nullptr };
    Structure* instanceStructure { nullptr };
    std::vector<BunEmbedRegisteredMethod> methods;
    std::vector<BunEmbedRegisteredProperty> properties;
};

static std::unordered_map<JSObject*, BunEmbedRegisteredMethod*> s_class_method_map;

static bool stringEqualsPropertyName(const std::string& name, PropertyName propertyName)
{
    ZigString zig_name = Zig::toZigString(propertyName.publicName());
    if (zig_name.len != name.size())
        return false;

    if (zig_name.len == 0)
        return true;

    return std::memcmp(name.data(), zig_name.ptr, zig_name.len) == 0;
}

class JSBunClassInstance final : public JSDestructibleObject {
public:
    using Base = JSDestructibleObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    static JSBunClassInstance* create(
        VM& vm,
        Structure* structure,
        BunEmbedRegisteredClass* registeredClass,
        void* nativePtr,
        BunEmbedClassFinalizerFn finalizer,
        void* userdata)
    {
        auto* instance = new (NotNull, allocateCell<JSBunClassInstance>(vm)) JSBunClassInstance(vm, structure, registeredClass, nativePtr, finalizer, userdata);
        instance->finishCreation(vm);
        return instance;
    }

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSObject* prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    static void destroy(JSCell* cell)
    {
        static_cast<JSBunClassInstance*>(cell)->~JSBunClassInstance();
    }

    template<typename, JSC::SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        if constexpr (mode == SubspaceAccess::Concurrently)
            return nullptr;
        return &vm.plainObjectSpace();
    }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    BunEmbedRegisteredClass* registeredClass() const { return m_registeredClass; }
    void* nativePtr() const { return m_nativePtr; }
    bool isDisposed() const { return m_disposed; }

    bool isInstanceOf(const BunEmbedRegisteredClass* expected) const
    {
        for (auto* current = m_registeredClass; current; current = current->parent) {
            if (current == expected)
                return true;
        }
        return false;
    }

    bool dispose()
    {
        if (m_disposed)
            return false;

        m_disposed = true;
        void* nativePtr = m_nativePtr;
        void* userdata = m_userdata;
        auto finalizer = m_finalizer;

        m_nativePtr = nullptr;
        m_userdata = nullptr;
        m_finalizer = nullptr;

        if (finalizer)
            finalizer(nativePtr, userdata);

        return true;
    }

private:
    JSBunClassInstance(
        VM& vm,
        Structure* structure,
        BunEmbedRegisteredClass* registeredClass,
        void* nativePtr,
        BunEmbedClassFinalizerFn finalizer,
        void* userdata)
        : Base(vm, structure)
        , m_registeredClass(registeredClass)
        , m_nativePtr(nativePtr)
        , m_userdata(userdata)
        , m_finalizer(finalizer)
    {
    }

    ~JSBunClassInstance()
    {
        dispose();
    }

    void finishCreation(VM& vm)
    {
        Base::finishCreation(vm);
    }

    BunEmbedRegisteredClass* m_registeredClass { nullptr };
    void* m_nativePtr { nullptr };
    void* m_userdata { nullptr };
    BunEmbedClassFinalizerFn m_finalizer { nullptr };
    bool m_disposed { false };
};

const ClassInfo JSBunClassInstance::s_info = { "BunEmbedClassInstance"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSBunClassInstance) };

template<typename Visitor>
void JSBunClassInstance::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = jsCast<JSBunClassInstance*>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
}

DEFINE_VISIT_CHILDREN(JSBunClassInstance);

static unsigned propertyAttributes(int readOnly, int dontEnum, int dontDelete)
{
    unsigned attributes = 0;
    if (readOnly)
        attributes |= PropertyAttribute::ReadOnly;
    if (dontEnum)
        attributes |= PropertyAttribute::DontEnum;
    if (dontDelete)
        attributes |= PropertyAttribute::DontDelete;
    return attributes;
}

static BunEmbedRegisteredProperty* findProperty(JSBunClassInstance* instance, PropertyName propertyName)
{
    for (auto* current = instance->registeredClass(); current; current = current->parent) {
        for (auto& property : current->properties) {
            if (stringEqualsPropertyName(property.name, propertyName))
                return &property;
        }
    }

    return nullptr;
}

JSC_DEFINE_CUSTOM_GETTER(BunEmbed_classGetter, (JSGlobalObject * globalObject, EncodedJSValue thisValue, PropertyName propertyName))
{
    auto* instance = jsDynamicCast<JSBunClassInstance*>(JSValue::decode(thisValue));
    if (!instance || instance->isDisposed())
        return JSValue::encode(jsUndefined());

    auto* property = findProperty(instance, propertyName);
    if (!property || !property->getter)
        return JSValue::encode(jsUndefined());

    return static_cast<EncodedJSValue>(property->getter(static_cast<void*>(globalObject), static_cast<uint64_t>(thisValue), instance->nativePtr(), property->userdata));
}

JSC_DEFINE_CUSTOM_SETTER(BunEmbed_classSetter, (JSGlobalObject * globalObject, EncodedJSValue thisValue, EncodedJSValue value, PropertyName propertyName))
{
    auto* instance = jsDynamicCast<JSBunClassInstance*>(JSValue::decode(thisValue));
    if (!instance || instance->isDisposed())
        return false;

    auto* property = findProperty(instance, propertyName);
    if (!property || !property->setter || (property->attributes & PropertyAttribute::ReadOnly))
        return false;

    property->setter(static_cast<void*>(globalObject), static_cast<uint64_t>(thisValue), instance->nativePtr(), static_cast<uint64_t>(value), property->userdata);
    return true;
}

JSC_DEFINE_HOST_FUNCTION(BunEmbed_classMethodDispatcher, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    auto* callee = jsDynamicCast<JSObject*>(callFrame->jsCallee());
    if (!callee)
        return JSValue::encode(jsUndefined());

    auto methodIt = s_class_method_map.find(callee);
    if (methodIt == s_class_method_map.end() || !methodIt->second || !methodIt->second->callback)
        return JSValue::encode(jsUndefined());

    auto* instance = jsDynamicCast<JSBunClassInstance*>(callFrame->thisValue());
    if (!instance || instance->isDisposed())
        return JSValue::encode(jsUndefined());

    const BunEmbedRegisteredMethod* method = methodIt->second;
    const size_t argCount = callFrame->argumentCount();
    WTF::Vector<uint64_t, 8> encodedArgs(argCount);
    for (size_t i = 0; i < argCount; ++i) {
        encodedArgs[i] = static_cast<uint64_t>(JSValue::encode(callFrame->uncheckedArgument(i)));
    }

    return static_cast<EncodedJSValue>(method->callback(
        static_cast<void*>(globalObject),
        static_cast<uint64_t>(JSValue::encode(callFrame->thisValue())),
        instance->nativePtr(),
        static_cast<int>(argCount),
        argCount == 0 ? nullptr : encodedArgs.mutableSpan().data(),
        method->userdata));
}

static void destroyRegisteredClass(BunEmbedRegisteredClass* registeredClass)
{
    if (!registeredClass)
        return;

    JSLockHolder lock(*registeredClass->vm);

    for (auto& method : registeredClass->methods) {
        if (method.functionObject)
            s_class_method_map.erase(method.functionObject);
    }

    if (registeredClass->instanceStructure)
        gcUnprotect(registeredClass->instanceStructure);
    if (registeredClass->prototype)
        gcUnprotect(registeredClass->prototype);

    delete registeredClass;
}

static bool isVMCompatible(JSGlobalObject* globalObject, BunEmbedRegisteredClass* registeredClass)
{
    return registeredClass && registeredClass->vm == &globalObject->vm();
}

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

static std::optional<uint32_t> bunTypedArrayKindFromType(JSC::TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
        return BunTypedArrayKindInt8;
    case TypeUint8:
        return BunTypedArrayKindUint8;
    case TypeUint8Clamped:
        return BunTypedArrayKindUint8C;
    case TypeInt16:
        return BunTypedArrayKindInt16;
    case TypeUint16:
        return BunTypedArrayKindUint16;
    case TypeInt32:
        return BunTypedArrayKindInt32;
    case TypeUint32:
        return BunTypedArrayKindUint32;
    case TypeFloat32:
        return BunTypedArrayKindFloat32;
    case TypeFloat64:
        return BunTypedArrayKindFloat64;
    case TypeBigInt64:
        return BunTypedArrayKindBigInt64;
    case TypeBigUint64:
        return BunTypedArrayKindBigUint64;
    default:
        return std::nullopt;
    }
}

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

extern "C" bool BunEmbed__getArrayBuffer(
    JSGlobalObject* globalObject,
    EncodedJSValue value,
    BunEmbedArrayBufferInfo* out)
{
    if (out)
        *out = {};
    if (!globalObject || !out)
        return false;

    auto* arrayBuffer = jsDynamicCast<JSArrayBuffer*>(JSValue::decode(value));
    if (!arrayBuffer)
        return false;

    auto* impl = arrayBuffer->impl();
    if (!impl || impl->isDetached())
        return false;

    out->data = impl->byteLength() > 0 ? impl->data() : nullptr;
    out->byte_length = impl->byteLength();
    return true;
}

extern "C" bool BunEmbed__getTypedArray(
    JSGlobalObject* globalObject,
    EncodedJSValue value,
    BunEmbedTypedArrayInfo* out)
{
    if (out)
        *out = {};
    if (!globalObject || !out)
        return false;

    JSValue jsValue = JSValue::decode(value);
    if (!jsValue.isCell() || !isTypedArrayType(jsValue.asCell()->type()))
        return false;

    auto* view = jsDynamicCast<JSArrayBufferView*>(jsValue);
    if (!view || view->isDetached())
        return false;

    auto kind = bunTypedArrayKindFromType(typedArrayType(view->type()));
    if (!kind)
        return false;

    out->data = view->byteLength() > 0 ? view->vector() : nullptr;
    out->byte_offset = view->byteOffset();
    out->byte_length = view->byteLength();
    out->element_count = view->length();
    out->kind = *kind;
    return true;
}

extern "C" BunEmbedRegisteredClass* BunEmbed__registerClass(
    JSGlobalObject* globalObject,
    const BunEmbedClassDescriptor* descriptor,
    BunEmbedRegisteredClass* parent)
{
    if (!globalObject || !descriptor || !descriptor->name || descriptor->name_len == 0)
        return nullptr;
    if (descriptor->property_count > 0 && !descriptor->properties)
        return nullptr;
    if (descriptor->method_count > 0 && !descriptor->methods)
        return nullptr;
    if (parent && !isVMCompatible(globalObject, parent))
        return nullptr;

    VM& vm = globalObject->vm();

    auto registeredClass = std::make_unique<BunEmbedRegisteredClass>();
    registeredClass->vm = &vm;
    registeredClass->parent = parent;
    registeredClass->name.assign(descriptor->name, descriptor->name_len);

    JSObject* parentPrototype = parent ? parent->prototype : globalObject->objectPrototype();
    JSObject* prototype = JSC::constructEmptyObject(globalObject, parentPrototype);
    if (!prototype)
        return nullptr;

    registeredClass->prototype = prototype;
    registeredClass->instanceStructure = JSBunClassInstance::createStructure(vm, globalObject, prototype);

    gcProtect(prototype);
    gcProtect(registeredClass->instanceStructure);

    registeredClass->properties.reserve(descriptor->property_count);
    for (size_t i = 0; i < descriptor->property_count; ++i) {
        const auto& property = descriptor->properties[i];
        if (!property.name || property.name_len == 0 || !property.getter)
            goto fail;

        WTF::String propertyName = WTF::String::fromUTF8(std::span { property.name, property.name_len });
        if (propertyName.isNull())
            goto fail;

        auto& dst = registeredClass->properties.emplace_back();
        dst.name.assign(property.name, property.name_len);
        dst.getter = property.getter;
        dst.setter = property.setter;
        dst.userdata = property.userdata;
        dst.attributes = propertyAttributes(property.read_only, property.dont_enum, property.dont_delete);

        auto* accessor = CustomGetterSetter::create(vm, BunEmbed_classGetter, property.setter && !property.read_only ? BunEmbed_classSetter : nullptr);
        prototype->putDirectCustomAccessor(vm, Identifier::fromString(vm, propertyName), accessor, dst.attributes);
    }

    registeredClass->methods.reserve(descriptor->method_count);
    for (size_t i = 0; i < descriptor->method_count; ++i) {
        const auto& method = descriptor->methods[i];
        if (!method.name || method.name_len == 0 || !method.callback)
            goto fail;

        WTF::String methodName = WTF::String::fromUTF8(std::span { method.name, method.name_len });
        if (methodName.isNull())
            goto fail;

        auto& dst = registeredClass->methods.emplace_back();
        dst.name.assign(method.name, method.name_len);
        dst.callback = method.callback;
        dst.userdata = method.userdata;
        dst.argCount = method.arg_count > 0 ? static_cast<unsigned>(method.arg_count) : 0;
        dst.attributes = propertyAttributes(0, method.dont_enum, method.dont_delete);
        dst.functionObject = JSFunction::create(vm, globalObject, dst.argCount, methodName, BunEmbed_classMethodDispatcher, ImplementationVisibility::Public);
        if (!dst.functionObject)
            goto fail;

        s_class_method_map[dst.functionObject] = &dst;
        prototype->putDirect(vm, Identifier::fromString(vm, methodName), dst.functionObject, dst.attributes);
    }

    return registeredClass.release();

fail:
    destroyRegisteredClass(registeredClass.release());
    return nullptr;
}

extern "C" void BunEmbed__destroyClass(BunEmbedRegisteredClass* registeredClass)
{
    destroyRegisteredClass(registeredClass);
}

extern "C" JSC::EncodedJSValue BunEmbed__createClassInstance(
    JSGlobalObject* globalObject,
    BunEmbedRegisteredClass* registeredClass,
    void* nativePtr,
    BunEmbedClassFinalizerFn finalizer,
    void* userdata)
{
    if (!globalObject || !isVMCompatible(globalObject, registeredClass))
        return {};

    VM& vm = globalObject->vm();
    auto* instance = JSBunClassInstance::create(vm, registeredClass->instanceStructure, registeredClass, nativePtr, finalizer, userdata);
    if (!instance)
        return {};

    return JSValue::encode(instance);
}

extern "C" void* BunEmbed__unwrapClassInstance(
    JSGlobalObject* globalObject,
    EncodedJSValue value,
    BunEmbedRegisteredClass* registeredClass)
{
    if (!globalObject)
        return nullptr;
    if (registeredClass && !isVMCompatible(globalObject, registeredClass))
        return nullptr;

    auto* instance = jsDynamicCast<JSBunClassInstance*>(JSValue::decode(value));
    if (!instance || instance->isDisposed())
        return nullptr;
    if (registeredClass && !instance->isInstanceOf(registeredClass))
        return nullptr;
    return instance->nativePtr();
}

extern "C" bool BunEmbed__isClassInstance(
    JSGlobalObject* globalObject,
    EncodedJSValue value)
{
    if (!globalObject)
        return false;
    return jsDynamicCast<JSBunClassInstance*>(JSValue::decode(value)) != nullptr;
}

extern "C" bool BunEmbed__instanceofClass(
    JSGlobalObject* globalObject,
    EncodedJSValue value,
    BunEmbedRegisteredClass* registeredClass)
{
    if (!globalObject || !isVMCompatible(globalObject, registeredClass))
        return false;

    auto* instance = jsDynamicCast<JSBunClassInstance*>(JSValue::decode(value));
    return instance && instance->isInstanceOf(registeredClass);
}

extern "C" bool BunEmbed__disposeClassInstance(
    JSGlobalObject* globalObject,
    EncodedJSValue value)
{
    if (!globalObject)
        return false;

    auto* instance = jsDynamicCast<JSBunClassInstance*>(JSValue::decode(value));
    if (!instance)
        return false;

    return instance->dispose();
}

extern "C" JSC::EncodedJSValue BunEmbed__classPrototype(
    JSGlobalObject* globalObject,
    BunEmbedRegisteredClass* registeredClass)
{
    if (!globalObject || !isVMCompatible(globalObject, registeredClass) || !registeredClass->prototype)
        return {};
    return JSValue::encode(registeredClass->prototype);
}

}
