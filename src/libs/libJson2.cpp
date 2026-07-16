#include "common/abi.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstring>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Libs {

namespace LibJson2 {

LIB_VERSION("Json2", 1, "Json2", 1, 1);

enum JsonValueType : uint32_t {
	JsonValueTypeNull = 0,
	JsonValueTypeBoolean,
	JsonValueTypeInteger,
	JsonValueTypeUInteger,
	JsonValueTypeReal,
	JsonValueTypeString,
	JsonValueTypeArray,
	JsonValueTypeObject,
};

constexpr int32_t JSON_ERROR_PARSE_INVALID_CHAR = -2138799871; // 0x80848101
constexpr int32_t JSON_ERROR_INVALID_ARGUMENT   = -2138799840; // 0x80848120

struct JsonValue;

struct JsonString {
	std::string* impl;
};

struct JsonArray {
	std::vector<JsonValue*>* impl;
};

struct JsonObject {
	std::map<std::string, JsonValue*>* impl;
};

struct JsonValue {
	JsonValue* parent;
	void*      rootparam;
	union {
		bool        boolean;
		int64_t     integer;
		uint64_t    uinteger;
		double      real;
		JsonString* string;
		JsonArray*  array;
		JsonObject* object;
	};
	char     padding[4];
	uint32_t type;
};

struct JsonInitParameter2 {
	void*    allocator;
	void*    user_data;
	size_t   file_buffer_size;
	uint32_t special_float_format_type;
	uint32_t reserved[3];
};

static std::string* JsonStringImpl(JsonString* self) {
	if (self == nullptr) {
		return nullptr;
	}
	if (self->impl == nullptr) {
		self->impl = new std::string;
	}
	return self->impl;
}

static const std::string* JsonStringImpl(const JsonString* self) {
	static const std::string empty;
	return (self != nullptr && self->impl != nullptr ? self->impl : &empty);
}

static std::vector<JsonValue*>* JsonArrayImpl(JsonArray* self) {
	if (self == nullptr) {
		return nullptr;
	}
	if (self->impl == nullptr) {
		self->impl = new std::vector<JsonValue*>;
	}
	return self->impl;
}

static const std::vector<JsonValue*>* JsonArrayImpl(const JsonArray* self) {
	static const std::vector<JsonValue*> empty;
	return (self != nullptr && self->impl != nullptr ? self->impl : &empty);
}

static std::map<std::string, JsonValue*>* JsonObjectImpl(JsonObject* self) {
	if (self == nullptr) {
		return nullptr;
	}
	if (self->impl == nullptr) {
		self->impl = new std::map<std::string, JsonValue*>;
	}
	return self->impl;
}

static const std::map<std::string, JsonValue*>* JsonObjectImpl(const JsonObject* self) {
	static const std::map<std::string, JsonValue*> empty;
	return (self != nullptr && self->impl != nullptr ? self->impl : &empty);
}

static JsonValue* JsonStaticNullValue() {
	static JsonValue value {};
	value.type = JsonValueTypeNull;
	return &value;
}

static JsonString* JsonStaticString() {
	static JsonString str {new std::string};
	return &str;
}

static JsonArray* JsonStaticArray() {
	static JsonArray array {new std::vector<JsonValue*>};
	return &array;
}

static JsonObject* JsonStaticObject() {
	static JsonObject object {new std::map<std::string, JsonValue*>};
	return &object;
}

static void JsonValueInit(JsonValue* self) {
	if (self != nullptr) {
		std::memset(self, 0, sizeof(JsonValue));
		self->type = JsonValueTypeNull;
	}
}

static void JsonValueClear(JsonValue* self);

static JsonValue* JsonValueNew() {
	auto* value = new JsonValue;
	JsonValueInit(value);
	return value;
}

static JsonString* JsonStringNew(const std::string& value) {
	auto* str = new JsonString {};
	str->impl = new std::string(value);
	return str;
}

static JsonArray* JsonArrayNew() {
	auto* array = new JsonArray {};
	array->impl = new std::vector<JsonValue*>;
	return array;
}

static JsonObject* JsonObjectNew() {
	auto* object = new JsonObject {};
	object->impl = new std::map<std::string, JsonValue*>;
	return object;
}

static void JsonValueDelete(JsonValue* self) {
	if (self != nullptr) {
		JsonValueClear(self);
		delete self;
	}
}

static void JsonStringDelete(JsonString* self) {
	if (self != nullptr) {
		delete self->impl;
		delete self;
	}
}

static void JsonArrayDelete(JsonArray* self) {
	if (self != nullptr) {
		if (self->impl != nullptr) {
			for (auto* value: *self->impl) {
				JsonValueDelete(value);
			}
		}
		delete self->impl;
		delete self;
	}
}

static void JsonObjectDelete(JsonObject* self) {
	if (self != nullptr) {
		if (self->impl != nullptr) {
			for (auto& item: *self->impl) {
				JsonValueDelete(item.second);
			}
		}
		delete self->impl;
		delete self;
	}
}

static void JsonValueClear(JsonValue* self) {
	if (self == nullptr) {
		return;
	}

	switch (self->type) {
		case JsonValueTypeString: JsonStringDelete(self->string); break;
		case JsonValueTypeArray: JsonArrayDelete(self->array); break;
		case JsonValueTypeObject: JsonObjectDelete(self->object); break;
		default: break;
	}

	self->parent    = nullptr;
	self->rootparam = nullptr;
	self->uinteger  = 0;
	self->type      = JsonValueTypeNull;
}

static void JsonValueCopy(JsonValue* dst, const JsonValue* src) {
	if (dst == nullptr || src == nullptr || dst == src) {
		return;
	}

	JsonValueClear(dst);
	dst->type = src->type;
	switch (src->type) {
		case JsonValueTypeBoolean: dst->boolean = src->boolean; break;
		case JsonValueTypeInteger: dst->integer = src->integer; break;
		case JsonValueTypeUInteger: dst->uinteger = src->uinteger; break;
		case JsonValueTypeReal: dst->real = src->real; break;
		case JsonValueTypeString: dst->string = JsonStringNew(*JsonStringImpl(src->string)); break;
		case JsonValueTypeArray: {
			dst->array = JsonArrayNew();
			for (auto* value: *JsonArrayImpl(src->array)) {
				auto* copy = JsonValueNew();
				JsonValueCopy(copy, value);
				copy->parent = dst;
				dst->array->impl->push_back(copy);
			}
			break;
		}
		case JsonValueTypeObject: {
			dst->object = JsonObjectNew();
			for (const auto& item: *JsonObjectImpl(src->object)) {
				auto* copy = JsonValueNew();
				JsonValueCopy(copy, item.second);
				copy->parent                     = dst;
				(*dst->object->impl)[item.first] = copy;
			}
			break;
		}
		default: dst->uinteger = 0; break;
	}
}

static JsonValue* JsonObjectLookup(JsonObject* object, const std::string& key, bool create) {
	auto* impl = JsonObjectImpl(object);
	if (impl == nullptr) {
		return JsonStaticNullValue();
	}
	auto it = impl->find(key);
	if (it != impl->end()) {
		return it->second;
	}
	if (!create) {
		return JsonStaticNullValue();
	}
	auto* value  = JsonValueNew();
	(*impl)[key] = value;
	return value;
}

static bool JsonValueFromNlohmann(JsonValue* out, const nlohmann::json& value) {
	if (out == nullptr) {
		return false;
	}

	JsonValueClear(out);
	if (value.is_null()) {
		return true;
	}
	if (value.is_boolean()) {
		out->type    = JsonValueTypeBoolean;
		out->boolean = value.get<bool>();
		return true;
	}
	if (value.is_number_integer() && !value.is_number_unsigned()) {
		out->type    = JsonValueTypeInteger;
		out->integer = value.get<int64_t>();
		return true;
	}
	if (value.is_number_unsigned()) {
		out->type     = JsonValueTypeUInteger;
		out->uinteger = value.get<uint64_t>();
		return true;
	}
	if (value.is_number_float()) {
		out->type = JsonValueTypeReal;
		out->real = value.get<double>();
		return true;
	}
	if (value.is_string()) {
		out->type   = JsonValueTypeString;
		out->string = JsonStringNew(value.get_ref<const std::string&>());
		return true;
	}
	if (value.is_array()) {
		out->type  = JsonValueTypeArray;
		out->array = JsonArrayNew();
		for (const auto& item: value) {
			auto* child = JsonValueNew();
			if (!JsonValueFromNlohmann(child, item)) {
				JsonValueDelete(child);
				return false;
			}
			child->parent = out;
			out->array->impl->push_back(child);
		}
		return true;
	}
	if (value.is_object()) {
		out->type   = JsonValueTypeObject;
		out->object = JsonObjectNew();
		for (auto it = value.begin(); it != value.end(); ++it) {
			auto* child = JsonValueNew();
			if (!JsonValueFromNlohmann(child, *it)) {
				JsonValueDelete(child);
				return false;
			}
			child->parent                  = out;
			(*out->object->impl)[it.key()] = child;
		}
		return true;
	}
	return false;
}

static void JsonSerializeValue(const JsonValue* value, std::string* out) {
	if (value == nullptr) {
		out->append("null");
		return;
	}
	switch (value->type) {
		case JsonValueTypeBoolean: out->append(value->boolean ? "true" : "false"); break;
		case JsonValueTypeInteger: out->append(std::to_string(value->integer)); break;
		case JsonValueTypeUInteger: out->append(std::to_string(value->uinteger)); break;
		case JsonValueTypeReal: out->append(std::to_string(value->real)); break;
		case JsonValueTypeString:
			out->push_back('"');
			out->append(*JsonStringImpl(value->string));
			out->push_back('"');
			break;
		case JsonValueTypeArray: {
			out->push_back('[');
			bool first = true;
			for (auto* item: *JsonArrayImpl(value->array)) {
				if (!first) {
					out->push_back(',');
				}
				first = false;
				JsonSerializeValue(item, out);
			}
			out->push_back(']');
			break;
		}
		case JsonValueTypeObject: {
			out->push_back('{');
			bool first = true;
			for (const auto& item: *JsonObjectImpl(value->object)) {
				if (!first) {
					out->push_back(',');
				}
				first = false;
				out->push_back('"');
				out->append(item.first);
				out->append("\":");
				JsonSerializeValue(item.second, out);
			}
			out->push_back('}');
			break;
		}
		default: out->append("null"); break;
	}
}

static void* KYTY_SYSV_ABI JsonMemAllocatorCtor(void* self) {
	PRINT_NAME();

	return self;
}

static JsonInitParameter2* KYTY_SYSV_ABI JsonInitParameter2Ctor(JsonInitParameter2* self) {
	PRINT_NAME();

	if (self != nullptr) {
		self->allocator                 = nullptr;
		self->user_data                 = nullptr;
		self->file_buffer_size          = 0;
		self->special_float_format_type = 0;
		self->reserved[0]               = 0;
		self->reserved[1]               = 0;
		self->reserved[2]               = 0;
	}

	return self;
}

static void KYTY_SYSV_ABI JsonInitParameter2SetAllocator(JsonInitParameter2* self, void* allocator,
                                                         void* user_data) {
	PRINT_NAME();

	if (self != nullptr) {
		self->allocator = allocator;
		self->user_data = user_data;
	}
}

static void KYTY_SYSV_ABI JsonInitParameter2SetFileBufferSize(JsonInitParameter2* self,
                                                              size_t              size) {
	PRINT_NAME();

	if (self != nullptr) {
		self->file_buffer_size = size;
	}
}

static void KYTY_SYSV_ABI JsonInitParameter2SetSpecialFloatFormatType(JsonInitParameter2* self,
                                                                      uint32_t            type) {
	PRINT_NAME();

	if (self != nullptr) {
		self->special_float_format_type = type;
	}
}

static void* KYTY_SYSV_ABI JsonInitializerCtor(void* self) {
	PRINT_NAME();

	return self;
}

static int KYTY_SYSV_ABI JsonInitializerInitialize(void*                     self,
                                                   const JsonInitParameter2* init_param) {
	PRINT_NAME();

	LOGF("\t self       = 0x%016" PRIx64 "\n"
	     "\t init_param = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(self), reinterpret_cast<uint64_t>(init_param));

	return 0;
}

static int KYTY_SYSV_ABI JsonInitializerInitializeV1(void* self, const void* init_param) {
	PRINT_NAME();

	LOGF("\t self       = 0x%016" PRIx64 "\n"
	     "\t init_param = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(self), reinterpret_cast<uint64_t>(init_param));

	return 0;
}

static void KYTY_SYSV_ABI JsonInitializerTerminate(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static void KYTY_SYSV_ABI JsonInitializerDtor(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static void KYTY_SYSV_ABI JsonMemAllocatorDtor(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static void* KYTY_SYSV_ABI JsonValueCtor(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));

	if (self != nullptr) {
		JsonValueInit(reinterpret_cast<JsonValue*>(self));
	}

	return self;
}

static JsonValue* KYTY_SYSV_ABI JsonValueBoolCtor(JsonValue* self, bool value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type    = JsonValueTypeBoolean;
		self->boolean = value;
	}
	return self;
}

static JsonValue* KYTY_SYSV_ABI JsonValueIntCtor(JsonValue* self, int64_t value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type    = JsonValueTypeInteger;
		self->integer = value;
	}
	return self;
}

static JsonValue* KYTY_SYSV_ABI JsonValueDoubleCtor(JsonValue* self, double value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type = JsonValueTypeReal;
		self->real = value;
	}
	return self;
}

static JsonValue* KYTY_SYSV_ABI JsonValueCStringCtor(JsonValue* self, const char* value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type   = JsonValueTypeString;
		self->string = JsonStringNew(value != nullptr ? value : "");
	}
	return self;
}

static JsonValue* KYTY_SYSV_ABI JsonValueStringCtor(JsonValue* self, const JsonString* value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type   = JsonValueTypeString;
		self->string = JsonStringNew(*JsonStringImpl(value));
	}
	return self;
}

static JsonValue* KYTY_SYSV_ABI JsonValueObjectCtor(JsonValue* self, const JsonObject* value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type   = JsonValueTypeObject;
		self->object = JsonObjectNew();
		for (const auto& item: *JsonObjectImpl(value)) {
			auto* copy = JsonValueNew();
			JsonValueCopy(copy, item.second);
			(*self->object->impl)[item.first] = copy;
		}
	}
	return self;
}

static void KYTY_SYSV_ABI JsonValueDtor(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));

	JsonValueClear(reinterpret_cast<JsonValue*>(self));
}

static JsonValue* KYTY_SYSV_ABI JsonValueAssign(JsonValue* self, const JsonValue* src) {
	PRINT_NAME();

	JsonValueCopy(self, src);
	return self;
}

static void KYTY_SYSV_ABI JsonValueSetBool(JsonValue* self, bool value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type    = JsonValueTypeBoolean;
		self->boolean = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetInt(JsonValue* self, int64_t value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type    = JsonValueTypeInteger;
		self->integer = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetUInt(JsonValue* self, uint64_t value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type     = JsonValueTypeUInteger;
		self->uinteger = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetDouble(JsonValue* self, double value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type = JsonValueTypeReal;
		self->real = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetString(JsonValue* self, const JsonString* value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type   = JsonValueTypeString;
		self->string = JsonStringNew(*JsonStringImpl(value));
	}
}

static void KYTY_SYSV_ABI JsonValueSetType(JsonValue* self, uint32_t type) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self == nullptr) {
		return;
	}
	self->type = type;
	switch (type) {
		case JsonValueTypeString: self->string = JsonStringNew(""); break;
		case JsonValueTypeArray: self->array = JsonArrayNew(); break;
		case JsonValueTypeObject: self->object = JsonObjectNew(); break;
		default: self->uinteger = 0; break;
	}
}

static uint32_t KYTY_SYSV_ABI JsonValueGetType(const JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr ? self->type : JsonValueTypeNull);
}

static JsonArray* KYTY_SYSV_ABI JsonValueReferArray(JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeArray ? self->array : nullptr);
}

static JsonObject* KYTY_SYSV_ABI JsonValueReferObject(JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeObject ? self->object : nullptr);
}

static const JsonString* KYTY_SYSV_ABI JsonValueGetString(const JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeString ? self->string
	                                                             : JsonStaticString());
}

static const JsonArray* KYTY_SYSV_ABI JsonValueGetArray(const JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeArray ? self->array : JsonStaticArray());
}

static const JsonObject* KYTY_SYSV_ABI JsonValueGetObject(const JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeObject ? self->object
	                                                             : JsonStaticObject());
}

static const bool* KYTY_SYSV_ABI JsonValueGetBoolean(const JsonValue* self) {
	PRINT_NAME();

	static const bool false_value = false;
	return (self != nullptr && self->type == JsonValueTypeBoolean ? &self->boolean : &false_value);
}

static const int64_t* KYTY_SYSV_ABI JsonValueGetInteger(const JsonValue* self) {
	PRINT_NAME();

	static const int64_t zero = 0;
	return (self != nullptr && self->type == JsonValueTypeInteger ? &self->integer : &zero);
}

static const double* KYTY_SYSV_ABI JsonValueGetReal(const JsonValue* self) {
	PRINT_NAME();

	static const double zero = 0.0;
	return (self != nullptr && self->type == JsonValueTypeReal ? &self->real : &zero);
}

static const JsonValue* KYTY_SYSV_ABI JsonValueIndexString(const JsonValue* self, const char* key) {
	PRINT_NAME();

	if (self == nullptr || self->type != JsonValueTypeObject) {
		return JsonStaticNullValue();
	}
	return JsonObjectLookup(self->object, key != nullptr ? key : "", false);
}

static const JsonValue* KYTY_SYSV_ABI JsonValueIndexUInt(const JsonValue* self, uint64_t index) {
	PRINT_NAME();

	if (self == nullptr || self->type != JsonValueTypeArray) {
		return JsonStaticNullValue();
	}
	const auto* impl = JsonArrayImpl(self->array);
	return (index < impl->size() ? (*impl)[static_cast<size_t>(index)] : JsonStaticNullValue());
}

static int32_t KYTY_SYSV_ABI JsonValueSerialize(JsonValue* self, JsonString* dst) {
	PRINT_NAME();

	auto* impl = JsonStringImpl(dst);
	if (impl != nullptr) {
		impl->clear();
		JsonSerializeValue(self, impl);
	}
	return 0;
}

static JsonString* KYTY_SYSV_ABI JsonStringCtor(JsonString* self) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = new std::string;
	}
	return self;
}

static JsonString* KYTY_SYSV_ABI JsonStringCStringCtor(JsonString* self, const char* str) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = new std::string(str != nullptr ? str : "");
	}
	return self;
}

static void KYTY_SYSV_ABI JsonStringDtor(JsonString* self) {
	PRINT_NAME();

	if (self != nullptr) {
		delete self->impl;
		self->impl = nullptr;
	}
}

static JsonString* KYTY_SYSV_ABI JsonStringAssign(JsonString* self, const JsonString* src) {
	PRINT_NAME();

	auto* impl = JsonStringImpl(self);
	if (impl != nullptr) {
		*impl = *JsonStringImpl(src);
	}
	return self;
}

static const char* KYTY_SYSV_ABI JsonStringCStr(const JsonString* self) {
	PRINT_NAME();

	return JsonStringImpl(self)->c_str();
}

static size_t KYTY_SYSV_ABI JsonStringLength(const JsonString* self) {
	PRINT_NAME();

	return JsonStringImpl(self)->length();
}

static JsonArray* KYTY_SYSV_ABI JsonArrayPushBack(JsonArray* self, const JsonValue* value) {
	PRINT_NAME();

	auto* impl = JsonArrayImpl(self);
	if (impl != nullptr) {
		auto* copy = JsonValueNew();
		JsonValueCopy(copy, value);
		impl->push_back(copy);
	}
	return self;
}

static JsonValue* KYTY_SYSV_ABI JsonArrayBack(const JsonArray* self) {
	PRINT_NAME();

	const auto* impl = JsonArrayImpl(self);
	return (!impl->empty() ? impl->back() : JsonStaticNullValue());
}

static size_t KYTY_SYSV_ABI JsonArraySize(const JsonArray* self) {
	PRINT_NAME();

	return JsonArrayImpl(self)->size();
}

static JsonObject* KYTY_SYSV_ABI JsonObjectCtor(JsonObject* self) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = new std::map<std::string, JsonValue*>;
	}
	return self;
}

static JsonObject* KYTY_SYSV_ABI JsonObjectCopyCtor(JsonObject* self, const JsonObject* src) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = new std::map<std::string, JsonValue*>;
		for (const auto& item: *JsonObjectImpl(src)) {
			auto* copy = JsonValueNew();
			JsonValueCopy(copy, item.second);
			(*self->impl)[item.first] = copy;
		}
	}
	return self;
}

static void KYTY_SYSV_ABI JsonObjectDtor(JsonObject* self) {
	PRINT_NAME();

	if (self != nullptr) {
		if (self->impl != nullptr) {
			for (auto& item: *self->impl) {
				JsonValueDelete(item.second);
			}
			self->impl->clear();
		}
		delete self->impl;
		self->impl = nullptr;
	}
}

static JsonObject* KYTY_SYSV_ABI JsonObjectAssign(JsonObject* self, const JsonObject* src) {
	PRINT_NAME();

	JsonObjectDtor(self);
	JsonObjectCopyCtor(self, src);
	return self;
}

static JsonValue* KYTY_SYSV_ABI JsonObjectIndex(JsonObject* self, const JsonString* key) {
	PRINT_NAME();

	return JsonObjectLookup(self, *JsonStringImpl(key), true);
}

static void KYTY_SYSV_ABI JsonObjectClear(JsonObject* self) {
	PRINT_NAME();

	auto* impl = JsonObjectImpl(self);
	if (impl != nullptr) {
		for (auto& item: *impl) {
			JsonValueDelete(item.second);
		}
		impl->clear();
	}
}

static int32_t KYTY_SYSV_ABI JsonParserParse(JsonValue* dst, const char* src, size_t size) {
	PRINT_NAME();

	if (dst == nullptr || src == nullptr) {
		return JSON_ERROR_INVALID_ARGUMENT;
	}

	JsonValue parsed {};
	JsonValueInit(&parsed);
	auto json = nlohmann::json::parse(src, src + size, nullptr, false);
	if (json.is_discarded() || !JsonValueFromNlohmann(&parsed, json)) {
		JsonValueClear(&parsed);
		return JSON_ERROR_PARSE_INVALID_CHAR;
	}

	JsonValueCopy(dst, &parsed);
	JsonValueClear(&parsed);
	return 0;
}

static int32_t KYTY_SYSV_ABI JsonInitializerSetGlobalNullAccessCallback(void* self, void* callback,
                                                                        void* context) {
	PRINT_NAME();

	LOGF("\t self     = 0x%016" PRIx64 "\n"
	     "\t callback = 0x%016" PRIx64 "\n"
	     "\t context  = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(self), reinterpret_cast<uint64_t>(callback),
	     reinterpret_cast<uint64_t>(context));

	return 0;
}

static int32_t KYTY_SYSV_ABI JsonValueCount(const JsonValue* self) {
	PRINT_NAME();

	if (self == nullptr) {
		return 0;
	}

	switch (self->type) {
		case JsonValueTypeArray: return static_cast<int32_t>(JsonArrayImpl(self->array)->size());
		case JsonValueTypeObject: return static_cast<int32_t>(JsonObjectImpl(self->object)->size());
		default: return 0;
	}
}

LIB_DEFINE(InitNet_1_Json2) {
	LIB_FUNC("-hJRce8wn1U", LibJson2::JsonMemAllocatorCtor);
	LIB_FUNC("WSOuge5IsCg", LibJson2::JsonInitParameter2Ctor);
	LIB_FUNC("GvGvswb0v34", LibJson2::JsonInitParameter2Ctor);
	LIB_FUNC("I2QC8PYhJWY", LibJson2::JsonInitParameter2SetAllocator);
	LIB_FUNC("W72B9ylU2JA", LibJson2::JsonInitParameter2SetAllocator);
	LIB_FUNC("Eu95jmqn5Rw", LibJson2::JsonInitParameter2SetFileBufferSize);
	LIB_FUNC("WVZBP4IyM+E", LibJson2::JsonInitParameter2SetSpecialFloatFormatType);
	LIB_FUNC("cK6bYHf-Q5E", LibJson2::JsonInitializerCtor);
	LIB_FUNC("IXW-z8pggfg", LibJson2::JsonInitializerInitialize);
	LIB_FUNC("Cxwy7wHq4J0", LibJson2::JsonInitializerInitializeV1);
	LIB_FUNC("PR5k1penBLM", LibJson2::JsonInitializerTerminate);
	LIB_FUNC("RujUxbr3haM", LibJson2::JsonInitializerDtor);
	LIB_FUNC("OcAgPxcq5Vk", LibJson2::JsonMemAllocatorDtor);
	LIB_FUNC("qBMjqyBn3OM", LibJson2::JsonValueCtor);
	LIB_FUNC("-wa17B7TGnw", LibJson2::JsonValueCtor);
	LIB_FUNC("WTtYf+cNnXI", LibJson2::JsonValueDtor);
	LIB_FUNC("0eUrW9JAxM0", LibJson2::JsonValueDtor);
	LIB_FUNC("S5JxQnoGF3E", LibJson2::JsonParserParse);
	LIB_FUNC("HwDt5lD9Bfo", LibJson2::JsonValueIndexString);
	LIB_FUNC("epJ6x2LV0kU", LibJson2::JsonValueGetString);
	LIB_FUNC("L1KAkYWml-M", LibJson2::JsonStringCStr);
	LIB_FUNC("OJPTonqdg0I", LibJson2::JsonObjectCtor);
	LIB_FUNC("sZIoMRGO+jk", LibJson2::JsonValueStringCtor);
	LIB_FUNC("ERuf9y0DY84", LibJson2::JsonObjectIndex);
	LIB_FUNC("4zrm6VrgIAw", LibJson2::JsonValueAssign);
	LIB_FUNC("a+W7HHlwpBs", LibJson2::JsonObjectCopyCtor);
	LIB_FUNC("5JmzZt8twAo", LibJson2::JsonObjectDtor);
	LIB_FUNC("nM5XqdeXFPw", LibJson2::JsonValueReferArray);
	LIB_FUNC("-NxEk7XLkDY", LibJson2::JsonValueReferObject);
	LIB_FUNC("zQtLRTqceMY", LibJson2::JsonArrayPushBack);
	LIB_FUNC("0lLK8+kDqmE", LibJson2::JsonValueIntCtor);
	LIB_FUNC("urOpESTBZmo", LibJson2::JsonObjectAssign);
	LIB_FUNC("zTwZdI8AZ5Y", LibJson2::JsonValueGetBoolean);
	LIB_FUNC("R7FDWtcN6f8", LibJson2::JsonValueSerialize);
	LIB_FUNC("oH8aBmLU+fc", LibJson2::JsonObjectClear);
	LIB_FUNC("bAM9Qwofus0", LibJson2::JsonArrayBack);
	LIB_FUNC("UeuWT+yNdCQ", LibJson2::JsonValueBoolCtor);
	LIB_FUNC("3xUXnmUkXfo", LibJson2::JsonValueObjectCtor);
	LIB_FUNC("cn9svYGWKDQ", LibJson2::JsonStringAssign);
	LIB_FUNC("b9V6fmppLXY", LibJson2::JsonValueCStringCtor);
	LIB_FUNC("EUH+EmT-v9E", LibJson2::JsonStringLength);
	LIB_FUNC("XlWbvieLj2M", LibJson2::JsonValueIndexUInt);
	LIB_FUNC("IlsmvBtMkak", LibJson2::JsonValueGetObject);
	LIB_FUNC("SHtAad20YYM", LibJson2::JsonValueGetType);
	LIB_FUNC("DIxvoy7Ngvk", LibJson2::JsonValueGetInteger);
	LIB_FUNC("qSmqLXXCPas", LibJson2::JsonStringCtor);
	LIB_FUNC("ONT8As5R1ug", LibJson2::JsonValueGetArray);
	LIB_FUNC("3qrge7L-AU4", LibJson2::JsonValueGetReal);
	LIB_FUNC("sOmU4vnx3s0", LibJson2::JsonValueDoubleCtor);
	LIB_FUNC("rQGJeNjOuUk", LibJson2::JsonArraySize);
	LIB_FUNC("5yHuiWXo2gg", LibJson2::JsonValueSetBool);
	LIB_FUNC("QxVVYhP-mvg", LibJson2::JsonValueSetInt);
	LIB_FUNC("SIe1ZmW7e7s", LibJson2::JsonValueSetUInt);
	LIB_FUNC("BSmWDIkV4w4", LibJson2::JsonValueSetDouble);
	LIB_FUNC("6l3Bv2gysNc", LibJson2::JsonValueSetString);
	LIB_FUNC("9KUZFjI1IxA", LibJson2::JsonStringCStringCtor);
	LIB_FUNC("cG1VE2HMl6c", LibJson2::JsonStringDtor);
	LIB_FUNC("IKQimvG9Wqs", LibJson2::JsonValueSetType);
	LIB_FUNC("RBw+4NukeGQ", LibJson2::JsonValueCount);
	LIB_FUNC("+drDFyAS6u4", LibJson2::JsonInitializerSetGlobalNullAccessCallback);
	LIB_FUNC("00oCq0RwSAY", LibJson2::JsonInitializerSetGlobalNullAccessCallback);
}

} // namespace LibJson2

} // namespace Libs
