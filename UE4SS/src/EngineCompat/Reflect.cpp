#include <EngineCompat/Reflect.hpp>

#include <algorithm>
#include <cwctype>
#include <format>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>

namespace RC::Compat::Reflect
{
    using namespace RC::Unreal;

    namespace
    {
        auto iequals(const StringType& a, const wchar_t* b) -> bool
        {
            size_t i = 0;
            for (; i < a.size() && b[i]; ++i)
            {
                if (std::towlower(a[i]) != std::towlower(b[i])) return false;
            }
            return i == a.size() && b[i] == L'\0';
        }

        // Calls fn(candidate) for every '|' separated alternative until fn returns true.
        template <typename Fn>
        auto for_each_alternative(const wchar_t* names, Fn&& fn) -> bool
        {
            const wchar_t* start = names;
            for (const wchar_t* p = names;; ++p)
            {
                if (*p == L'|' || *p == L'\0')
                {
                    if (p > start && fn(StringType(start, p))) return true;
                    if (*p == L'\0') break;
                    start = p + 1;
                }
            }
            return false;
        }

        auto find_by_name(UStruct* owner, const StringType& name) -> FProperty*
        {
            if (!owner) return nullptr;
            for (FProperty* p : TFieldRange<FProperty>(owner))
            {
                if (iequals(p->GetName(), name.c_str())) return p;
            }
            return nullptr;
        }

        auto struct_of(FProperty* property) -> UScriptStruct*
        {
            auto* sp = CastField<FStructProperty>(property);
            return sp ? sp->GetStruct() : nullptr;
        }
    } // namespace

    // ---- Lookup ----------------------------------------------------------------------------------

    auto FindClass(const StringType& name) -> UClass*
    {
        if (name.empty()) return nullptr;
        if (name.find(L'/') != StringType::npos) return UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, name);

        for (const wchar_t* prefix : {L"/Script/Engine.", L"/Script/CoreUObject.", L"/Script/UMG.", L"/Script/AIModule."})
        {
            if (auto* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, StringType(prefix) + name)) return cls;
        }
        for (const wchar_t* meta : {L"Class", L"BlueprintGeneratedClass"})
        {
            if (auto* obj = UObjectGlobals::FindObject(meta, name.c_str())) return static_cast<UClass*>(obj);
        }
        return nullptr;
    }

    auto DefaultObjectOf(const StringType& class_name) -> UObject*
    {
        auto* cls = FindClass(class_name);
        return cls ? cls->GetClassDefaultObject() : nullptr;
    }

    auto FindFunction(UObject* object, const StringType& name) -> UFunction*
    {
        if (!object || name.empty()) return nullptr;
        return object->GetFunctionByNameInChain(name.c_str());
    }

    auto FindFunctionByPath(const StringType& path) -> UFunction*
    {
        const auto colon = path.rfind(L':');
        if (colon == StringType::npos) return nullptr;
        auto* cls = FindClass(path.substr(0, colon));
        if (!cls) return nullptr;
        const StringType name = path.substr(colon + 1);
        // Search the class itself and its supers. (UObject::GetFunctionByNameInChain on a UClass would look at the
        // functions of UClass, not at the ones declared by it.)
        for (UStruct* s = cls; s; s = s->GetSuperStruct())
        {
            for (UFunction* fn : s->ForEachFunction())
            {
                if (fn->GetName() == name) return fn;
            }
        }
        return nullptr;
    }

    auto FindProperty(UObject* object, const StringType& name) -> FProperty*
    {
        if (!object) return nullptr;
        return find_by_name(object->GetClassPrivate(), name);
    }

    auto IsValidObject(const UObject* object) -> bool
    {
        return object && UObject::IsReal(object);
    }

    // ---- Struct fields ---------------------------------------------------------------------------

    auto GetStructNumber(UScriptStruct* type, const void* value, const wchar_t* field) -> std::optional<double>
    {
        if (!type || !value) return std::nullopt;
        FProperty* p = find_by_name(type, field);
        if (!p) return std::nullopt;
        return GetNumber(p, p->ContainerPtrToValuePtr<void>(value));
    }

    auto SetStructNumber(UScriptStruct* type, void* value, const wchar_t* field, double number) -> bool
    {
        if (!type || !value) return false;
        FProperty* p = find_by_name(type, field);
        return p && SetNumber(p, p->ContainerPtrToValuePtr<void>(value), number);
    }

    auto ReadVec3(FProperty* property, const void* value, Vec3& out) -> bool
    {
        auto* type = struct_of(property);
        auto x = GetStructNumber(type, value, L"X");
        auto y = GetStructNumber(type, value, L"Y");
        auto z = GetStructNumber(type, value, L"Z");
        if (!x || !y || !z) return false;
        out = {*x, *y, *z};
        return true;
    }

    auto WriteVec3(FProperty* property, void* value, const Vec3& in) -> bool
    {
        auto* type = struct_of(property);
        return SetStructNumber(type, value, L"X", in.x) & SetStructNumber(type, value, L"Y", in.y) & SetStructNumber(type, value, L"Z", in.z);
    }

    auto ReadVec2(FProperty* property, const void* value, Vec2& out) -> bool
    {
        auto* type = struct_of(property);
        auto x = GetStructNumber(type, value, L"X");
        auto y = GetStructNumber(type, value, L"Y");
        if (!x || !y) return false;
        out = {*x, *y};
        return true;
    }

    auto WriteVec2(FProperty* property, void* value, const Vec2& in) -> bool
    {
        auto* type = struct_of(property);
        return SetStructNumber(type, value, L"X", in.x) & SetStructNumber(type, value, L"Y", in.y);
    }

    auto ReadRot3(FProperty* property, const void* value, Rot3& out) -> bool
    {
        auto* type = struct_of(property);
        auto p = GetStructNumber(type, value, L"Pitch");
        auto y = GetStructNumber(type, value, L"Yaw");
        auto r = GetStructNumber(type, value, L"Roll");
        if (!p || !y || !r) return false;
        out = {*p, *y, *r};
        return true;
    }

    auto WriteRot3(FProperty* property, void* value, const Rot3& in) -> bool
    {
        auto* type = struct_of(property);
        return SetStructNumber(type, value, L"Pitch", in.pitch) & SetStructNumber(type, value, L"Yaw", in.yaw) &
               SetStructNumber(type, value, L"Roll", in.roll);
    }

    auto WriteColor(FProperty* property, void* value, const Color& in) -> bool
    {
        auto* type = struct_of(property);
        return SetStructNumber(type, value, L"R", in.r) & SetStructNumber(type, value, L"G", in.g) & SetStructNumber(type, value, L"B", in.b) &
               SetStructNumber(type, value, L"A", in.a);
    }

    // ---- Generic values --------------------------------------------------------------------------

    // The engine's own FProperty::ExportText/ImportText are virtual functions UE4SS cannot always resolve (they are
    // missing on 4.27, for one), so values are converted by walking the reflection data here. The text form follows
    // the engine's: "(X=1,Y=2,Z=3)" for structs, "True"/"False", enum entry names, object path names.
    namespace
    {
        constexpr int kMaxDepth = 4;

        auto to_wstring_number(double v, bool single_precision) -> StringType
        {
            return single_precision ? std::format(L"{}", static_cast<float>(v)) : std::format(L"{}", v);
        }

        auto trim(StringType text) -> StringType
        {
            const auto b = text.find_first_not_of(L" \t\r\n");
            if (b == StringType::npos) return {};
            const auto e = text.find_last_not_of(L" \t\r\n");
            return text.substr(b, e - b + 1);
        }

        auto enum_of(FProperty* property) -> UEnum*
        {
            if (auto* b = CastField<FByteProperty>(property)) return b->GetEnum();
            if (auto* e = CastField<FEnumProperty>(property)) return e->GetEnum();
            return nullptr;
        }

        auto enum_entry_name(UEnum* e, int64 number) -> std::optional<StringType>
        {
            std::vector<std::pair<FName, int64>> names;
            e->GetEnumNamesAsVector(names);
            for (auto& [name, entry_value] : names)
            {
                if (entry_value == number)
                {
                    StringType full = name.ToString();
                    const auto sep = full.rfind(L"::");
                    return sep == StringType::npos ? full : full.substr(sep + 2);
                }
            }
            return std::nullopt;
        }

        auto export_value(FProperty* p, const void* v, UObject* parent, int depth) -> StringType
        {
            if (!p || !v) return L"None";
            if (auto* b = CastField<FBoolProperty>(p)) return b->GetPropertyValue(v) ? L"True" : L"False";

            if (UEnum* e = enum_of(p))
            {
                auto number = GetNumber(p, v);
                if (number)
                {
                    if (auto name = enum_entry_name(e, static_cast<int64>(*number))) return *name;
                    return std::format(L"{}", static_cast<int64>(*number));
                }
            }
            if (auto* n = CastField<FNumericProperty>(p))
            {
                if (n->IsFloatingPoint()) return to_wstring_number(n->GetFloatingPointPropertyValue(v), CastField<FFloatProperty>(p) != nullptr);
                if (CastField<FUInt64Property>(p)) return std::format(L"{}", n->GetUnsignedIntPropertyValue(v));
                return std::format(L"{}", n->GetSignedIntPropertyValue(const_cast<void*>(v)));
            }
            if (CastField<FEnumProperty>(p))
            {
                auto number = GetNumber(p, v);
                return number ? std::format(L"{}", static_cast<int64>(*number)) : StringType(L"?");
            }
            if (auto* n = CastField<FNameProperty>(p)) return n->GetPropertyValue(v).ToString();
            if (auto* s = CastField<FStrProperty>(p))
            {
                const wchar_t* chars = s->GetPropertyValue(v).GetCharArray().GetData();
                return chars ? StringType(chars) : StringType{};
            }
            if (auto* t = CastField<FTextProperty>(p)) return t->GetPropertyValue(v).ToString();
            if (auto* o = CastField<FObjectPropertyBase>(p))
            {
                UObject* obj = o->GetObjectPropertyValue(v);
                return obj ? obj->GetPathName() : StringType(L"None");
            }
            if (auto* sp = CastField<FStructProperty>(p))
            {
                if (depth >= kMaxDepth || !sp->GetStruct()) return L"(...)";
                StringType out = L"(";
                bool first = true;
                for (FProperty* field : TFieldRange<FProperty>(sp->GetStruct()))
                {
                    if (!first) out += L",";
                    first = false;
                    out += field->GetName() + L"=" + export_value(field, field->ContainerPtrToValuePtr<void>(v), parent, depth + 1);
                }
                return out + L")";
            }
            if (CastField<FArrayProperty>(p)) return std::format(L"[{} items]", static_cast<const FScriptArray*>(v)->Num());
            if (CastField<FMapProperty>(p)) return L"{map}";
            if (CastField<FSetProperty>(p)) return L"{set}";
            return L"<" + p->GetClass().GetName() + L">";
        }

        auto parse_bool(const StringType& text, bool& out) -> bool
        {
            StringType t = text;
            std::transform(t.begin(), t.end(), t.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (t == L"true" || t == L"1" || t == L"yes" || t == L"on") { out = true; return true; }
            if (t == L"false" || t == L"0" || t == L"no" || t == L"off") { out = false; return true; }
            return false;
        }

        auto parse_double(const StringType& text, double& out) -> bool
        {
            if (text.empty()) return false;
            wchar_t* end = nullptr;
            out = std::wcstod(text.c_str(), &end);
            return end && *end == L'\0';
        }

        // Splits "(A=1,B=(C=2,D=3))" into top-level key/value pairs.
        auto split_struct_text(const StringType& text) -> std::vector<std::pair<StringType, StringType>>
        {
            std::vector<std::pair<StringType, StringType>> pairs;
            if (text.size() < 2 || text.front() != L'(' || text.back() != L')') return pairs;
            const StringType inner = text.substr(1, text.size() - 2);
            int depth = 0;
            bool quoted = false;
            size_t start = 0;
            auto flush = [&](size_t end) {
                StringType item = inner.substr(start, end - start);
                const auto eq = item.find(L'=');
                if (eq != StringType::npos) pairs.emplace_back(trim(item.substr(0, eq)), trim(item.substr(eq + 1)));
            };
            for (size_t i = 0; i < inner.size(); ++i)
            {
                const wchar_t c = inner[i];
                if (c == L'"') quoted = !quoted;
                else if (!quoted && (c == L'(' || c == L'[' || c == L'{')) ++depth;
                else if (!quoted && (c == L')' || c == L']' || c == L'}')) --depth;
                else if (!quoted && depth == 0 && c == L',')
                {
                    flush(i);
                    start = i + 1;
                }
            }
            flush(inner.size());
            return pairs;
        }

        auto import_value(FProperty* p, void* v, StringType text, UObject* owner, int depth) -> bool
        {
            if (!p || !v) return false;
            text = trim(std::move(text));

            if (CastField<FBoolProperty>(p))
            {
                bool b = false;
                if (!parse_bool(text, b)) return false;
                return SetNumber(p, v, b ? 1.0 : 0.0);
            }
            if (UEnum* e = enum_of(p))
            {
                double number = 0;
                if (parse_double(text, number)) return SetNumber(p, v, number);
                std::vector<std::pair<FName, int64>> names;
                e->GetEnumNamesAsVector(names);
                StringType wanted = text;
                if (const auto sep = wanted.rfind(L"::"); sep != StringType::npos) wanted = wanted.substr(sep + 2);
                auto lower = [](StringType s) {
                    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    return s;
                };
                for (auto& [name, entry_value] : names)
                {
                    StringType full = name.ToString();
                    if (const auto sep = full.rfind(L"::"); sep != StringType::npos) full = full.substr(sep + 2);
                    if (lower(full) == lower(wanted)) return SetNumber(p, v, static_cast<double>(entry_value));
                }
                return false;
            }
            if (IsNumeric(p))
            {
                double number = 0;
                if (!parse_double(text, number)) return false;
                return SetNumber(p, v, number);
            }
            if (auto* n = CastField<FNameProperty>(p))
            {
                n->SetPropertyValue(v, FName(text.c_str()));
                return true;
            }
            if (CastField<FStrProperty>(p))
            {
                *static_cast<FString*>(v) = FString(text.c_str());
                return true;
            }
            if (auto* o = CastField<FObjectPropertyBase>(p))
            {
                if (text.empty() || text == L"None" || text == L"null")
                {
                    o->SetObjectPropertyValue(v, nullptr);
                    return true;
                }
                // Accept the engine's "Class'/Path.Name'" form.
                if (const auto q = text.find(L'\''); q != StringType::npos && text.back() == L'\'') text = text.substr(q + 1, text.size() - q - 2);
                UObject* obj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, text);
                if (!obj) return false;
                o->SetObjectPropertyValue(v, obj);
                return true;
            }
            if (auto* sp = CastField<FStructProperty>(p))
            {
                if (depth >= kMaxDepth || !sp->GetStruct()) return false;
                bool any = false;
                for (auto& [name, field_text] : split_struct_text(text))
                {
                    FProperty* field = find_by_name(sp->GetStruct(), name);
                    if (!field) return false;
                    if (!import_value(field, field->ContainerPtrToValuePtr<void>(v), field_text, owner, depth + 1)) return false;
                    any = true;
                }
                return any;
            }
            return false;
        }
    } // namespace

    auto ExportText(FProperty* property, const void* value, UObject* parent) -> std::optional<StringType>
    {
        if (!property || !value) return std::nullopt;
        return export_value(property, value, parent, 0);
    }

    auto ImportText(FProperty* property, void* value, const StringType& text, UObject* owner) -> bool
    {
        return import_value(property, value, text, owner, 0);
    }

    auto IsNumeric(FProperty* property) -> bool
    {
        return CastField<FBoolProperty>(property) || CastField<FNumericProperty>(property) || CastField<FEnumProperty>(property);
    }

    auto GetNumber(FProperty* property, const void* value) -> std::optional<double>
    {
        if (!property || !value) return std::nullopt;
        if (auto* b = CastField<FBoolProperty>(property)) return b->GetPropertyValue(value) ? 1.0 : 0.0;
        if (auto* n = CastField<FNumericProperty>(property))
        {
            if (n->IsFloatingPoint()) return n->GetFloatingPointPropertyValue(value);
            if (n->IsInteger()) return static_cast<double>(n->GetSignedIntPropertyValue(const_cast<void*>(value)));
        }
        if (auto* e = CastField<FEnumProperty>(property))
        {
            if (auto* under = e->GetUnderlyingProperty()) return static_cast<double>(under->GetSignedIntPropertyValue(const_cast<void*>(value)));
        }
        return std::nullopt;
    }

    auto SetNumber(FProperty* property, void* value, double number) -> bool
    {
        if (!property || !value) return false;
        if (auto* b = CastField<FBoolProperty>(property))
        {
            b->SetPropertyValue(value, number != 0.0);
            return true;
        }
        if (auto* n = CastField<FNumericProperty>(property))
        {
            if (n->IsFloatingPoint())
            {
                n->SetFloatingPointPropertyValue(value, number);
                return true;
            }
            if (n->IsInteger())
            {
                n->SetIntPropertyValue(value, static_cast<int64>(number));
                return true;
            }
        }
        if (auto* e = CastField<FEnumProperty>(property))
        {
            if (auto* under = e->GetUnderlyingProperty())
            {
                under->SetIntPropertyValue(value, static_cast<int64>(number));
                return true;
            }
        }
        return false;
    }

    auto GetPropertyText(UObject* object, const StringType& name) -> std::optional<StringType>
    {
        FProperty* p = FindProperty(object, name);
        return p ? ExportText(p, p->ContainerPtrToValuePtr<void>(object), object) : std::nullopt;
    }

    auto SetPropertyText(UObject* object, const StringType& name, const StringType& text) -> bool
    {
        FProperty* p = FindProperty(object, name);
        return p && ImportText(p, p->ContainerPtrToValuePtr<void>(object), text, object);
    }

    auto GetPropertyNumber(UObject* object, const StringType& name) -> std::optional<double>
    {
        FProperty* p = FindProperty(object, name);
        return p ? GetNumber(p, p->ContainerPtrToValuePtr<void>(object)) : std::nullopt;
    }

    auto SetPropertyNumber(UObject* object, const StringType& name, double number) -> bool
    {
        FProperty* p = FindProperty(object, name);
        return p && SetNumber(p, p->ContainerPtrToValuePtr<void>(object), number);
    }

    auto GetObjectProperty(UObject* object, const StringType& name) -> UObject*
    {
        FProperty* p = FindProperty(object, name);
        auto* op = CastField<FObjectPropertyBase>(p);
        if (!op) return nullptr;
        return op->GetObjectPropertyValue(p->ContainerPtrToValuePtr<void>(object));
    }

    auto TypeName(FProperty* property) -> StringType
    {
        if (!property) return L"?";
        if (auto* sp = CastField<FStructProperty>(property)) return sp->GetStruct() ? sp->GetStruct()->GetName() : L"Struct";
        if (auto* cp = CastField<FClassProperty>(property)) return L"Class<" + (cp->GetMetaClass() ? cp->GetMetaClass()->GetName() : StringType(L"?")) + L">";
        if (auto* op = CastField<FObjectPropertyBase>(property))
            return L"Object<" + (op->GetPropertyClass() ? op->GetPropertyClass()->GetName() : StringType(L"?")) + L">";
        if (auto* ap = CastField<FArrayProperty>(property)) return L"Array<" + TypeName(ap->GetInner()) + L">";
        StringType cls = property->GetClass().GetName();
        // "FloatProperty" -> "float" style short names for the common cases.
        if (cls == L"FloatProperty") return L"float";
        if (cls == L"DoubleProperty") return L"double";
        if (cls == L"IntProperty") return L"int";
        if (cls == L"Int64Property") return L"int64";
        if (cls == L"BoolProperty") return L"bool";
        if (cls == L"ByteProperty") return L"byte";
        if (cls == L"StrProperty") return L"String";
        if (cls == L"NameProperty") return L"Name";
        if (cls == L"TextProperty") return L"Text";
        if (cls == L"EnumProperty") return L"enum";
        return cls;
    }

    // ---- Call ------------------------------------------------------------------------------------

    Call::Call(UObject* target, UFunction* function) : m_target(target), m_function(function)
    {
        if (!valid()) return;
        m_buffer.assign(std::max<size_t>(function->GetParmsSize(), 1), 0);
        for (FProperty* p : TFieldRange<FProperty>(function))
        {
            if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;
            p->InitializeValue(m_buffer.data() + p->GetOffset_Internal());
            m_params.push_back(p);
        }
    }

    auto Call::Static(const StringType& class_name, const StringType& function_name) -> Call
    {
        UObject* cdo = DefaultObjectOf(class_name);
        return Call(cdo, FindFunction(cdo, function_name));
    }

    Call::~Call()
    {
        for (FProperty* p : m_params) p->DestroyValue(m_buffer.data() + p->GetOffset_Internal());
    }

    Call::Call(Call&& other) noexcept
        : m_target(other.m_target), m_function(other.m_function), m_buffer(std::move(other.m_buffer)), m_params(std::move(other.m_params))
    {
        other.m_params.clear();
        other.m_function = nullptr;
    }

    auto Call::find_param(const wchar_t* names) const -> FProperty*
    {
        FProperty* found = nullptr;
        for_each_alternative(names, [&](const StringType& candidate) {
            for (FProperty* p : m_params)
            {
                if (iequals(p->GetName(), candidate.c_str()))
                {
                    found = p;
                    return true;
                }
            }
            return false;
        });
        return found;
    }

    auto Call::param_ptr(FProperty* property) const -> void*
    {
        return property ? const_cast<uint8_t*>(m_buffer.data()) + property->GetOffset_Internal() : nullptr;
    }

    auto Call::has_param(const wchar_t* names) const -> bool
    {
        return find_param(names) != nullptr;
    }

    auto Call::SetBool(const wchar_t* names, bool value) -> Call&
    {
        return SetNumber(names, value ? 1.0 : 0.0);
    }

    auto Call::SetNumber(const wchar_t* names, double value) -> Call&
    {
        if (FProperty* p = find_param(names)) Reflect::SetNumber(p, param_ptr(p), value);
        return *this;
    }

    auto Call::SetObject(const wchar_t* names, UObject* value) -> Call&
    {
        if (auto* op = CastField<FObjectPropertyBase>(find_param(names))) op->SetObjectPropertyValue(param_ptr(op), value);
        return *this;
    }

    auto Call::SetText(const wchar_t* names, const StringType& text) -> Call&
    {
        if (FProperty* p = find_param(names)) Reflect::ImportText(p, param_ptr(p), text, m_target);
        return *this;
    }

    auto Call::SetVec3(const wchar_t* names, const Vec3& value) -> Call&
    {
        if (FProperty* p = find_param(names)) WriteVec3(p, param_ptr(p), value);
        return *this;
    }

    auto Call::SetVec2(const wchar_t* names, const Vec2& value) -> Call&
    {
        if (FProperty* p = find_param(names)) WriteVec2(p, param_ptr(p), value);
        return *this;
    }

    auto Call::SetRot3(const wchar_t* names, const Rot3& value) -> Call&
    {
        if (FProperty* p = find_param(names)) WriteRot3(p, param_ptr(p), value);
        return *this;
    }

    auto Call::SetColor(const wchar_t* names, const Color& value) -> Call&
    {
        if (FProperty* p = find_param(names)) WriteColor(p, param_ptr(p), value);
        return *this;
    }

    auto Call::Invoke() -> bool
    {
        if (!valid() || !IsValidObject(m_target)) return false;
        try
        {
            m_target->ProcessEvent(m_function, m_buffer.data());
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    auto Call::GetBool(const wchar_t* names) const -> std::optional<bool>
    {
        auto n = GetNumber(names);
        return n ? std::optional<bool>(*n != 0.0) : std::nullopt;
    }

    auto Call::GetNumber(const wchar_t* names) const -> std::optional<double>
    {
        FProperty* p = find_param(names);
        return p ? Reflect::GetNumber(p, param_ptr(p)) : std::nullopt;
    }

    auto Call::GetObject(const wchar_t* names) const -> UObject*
    {
        auto* op = CastField<FObjectPropertyBase>(find_param(names));
        return op ? op->GetObjectPropertyValue(param_ptr(op)) : nullptr;
    }

    auto Call::GetText(const wchar_t* names) const -> std::optional<StringType>
    {
        FProperty* p = find_param(names);
        if (!p) return std::nullopt;
        if (auto* sp = CastField<FStrProperty>(p))
        {
            const FString& s = sp->GetPropertyValue(param_ptr(p));
            const wchar_t* chars = s.GetCharArray().GetData();
            return chars ? StringType(chars) : StringType{};
        }
        return Reflect::ExportText(p, param_ptr(p), m_target);
    }

    auto Call::GetVec3(const wchar_t* names) const -> std::optional<Vec3>
    {
        FProperty* p = find_param(names);
        Vec3 v;
        return p && ReadVec3(p, param_ptr(p), v) ? std::optional<Vec3>(v) : std::nullopt;
    }

    auto Call::GetVec2(const wchar_t* names) const -> std::optional<Vec2>
    {
        FProperty* p = find_param(names);
        Vec2 v;
        return p && ReadVec2(p, param_ptr(p), v) ? std::optional<Vec2>(v) : std::nullopt;
    }

    auto Call::describe() const -> StringType
    {
        if (!m_function) return L"<invalid call>";
        StringType out = m_function->GetName() + L"(";
        bool first = true;
        for (FProperty* p : m_params)
        {
            if (!first) out += L", ";
            first = false;
            out += TypeName(p) + L" " + p->GetName();
        }
        return out + L")";
    }
} // namespace RC::Compat::Reflect
