#include <Trainer/Inspect.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include <EngineCompat/GameThread.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Trainer/TargetResolver.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UnrealFlags.hpp>

namespace RC::Toolkit
{
    using namespace RC::Unreal;
    namespace Reflect = RC::Compat::Reflect;

    auto inspect_target(const std::string& expression) -> TargetInfo
    {
        TargetInfo info;
        info.error = "timed out waiting for the game thread";
        Compat::GameThread::Run([&] {
            try
            {
                auto objects = resolve_targets(expression);
                if (objects.empty())
                {
                    info.error = "target '" + expression + "' not found (is a level loaded?)";
                    return;
                }
                UObject* obj = objects.front();
                info.found = true;
                info.error.clear();
                info.object_name = to_string(obj->GetName());
                info.class_name = to_string(obj->GetClassPrivate()->GetName());
                if (objects.size() > 1) info.object_name += " (+" + std::to_string(objects.size() - 1) + " more)";

                for (FProperty* p : TFieldRange<FProperty>(obj->GetClassPrivate()))
                {
                    PropRow row;
                    row.name = to_string(p->GetName());
                    row.type = to_string(Reflect::TypeName(p));
                    row.numeric = Reflect::IsNumeric(p);
                    row.is_object = CastField<FObjectPropertyBase>(p) != nullptr;
                    if (auto text = Reflect::ExportText(p, p->ContainerPtrToValuePtr<void>(obj), obj))
                    {
                        row.value = to_string(*text);
                        if (row.value.size() > 160) row.value = row.value.substr(0, 157) + "...";
                    }
                    info.properties.push_back(std::move(row));
                }

                for (UStruct* s = obj->GetClassPrivate(); s; s = s->GetSuperStruct())
                {
                    for (UFunction* fn : s->ForEachFunction())
                    {
                        FuncRow row;
                        row.name = to_string(fn->GetName());
                        row.owner = to_string(s->GetName());
                        std::string sig = "(";
                        bool first = true;
                        for (FProperty* p : TFieldRange<FProperty>(fn))
                        {
                            if (!p->HasAnyPropertyFlags(CPF_Parm) || p->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
                            if (!first) sig += ", ";
                            first = false;
                            sig += to_string(Reflect::TypeName(p)) + " " + to_string(p->GetName());
                        }
                        row.signature = sig + ")";
                        info.functions.push_back(std::move(row));
                    }
                }
                std::sort(info.properties.begin(), info.properties.end(), [](const PropRow& a, const PropRow& b) { return a.name < b.name; });
                std::sort(info.functions.begin(), info.functions.end(), [](const FuncRow& a, const FuncRow& b) { return a.name < b.name; });
            }
            catch (...)
            {
                info.error = "exception while inspecting target";
            }
        });
        return info;
    }
} // namespace RC::Toolkit

namespace RC::Toolkit
{
    auto list_live_classes(bool include_non_actors) -> std::vector<ClassRow>
    {
        std::vector<ClassRow> rows;
        Compat::GameThread::Run(
                [&] {
                    std::unordered_map<UClass*, size_t> index;
                    UClass* actor_class = AActor::StaticClass();
                    UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
                        if (!obj || obj->HasAnyFlags(RF_ClassDefaultObject)) return LoopAction::Continue;
                        UClass* cls = obj->GetClassPrivate();
                        if (!cls) return LoopAction::Continue;
                        auto it = index.find(cls);
                        if (it != index.end())
                        {
                            ++rows[it->second].instances;
                            return LoopAction::Continue;
                        }
                        const bool is_actor = cls->IsChildOf(actor_class);
                        if (!is_actor && !include_non_actors) return LoopAction::Continue;
                        const std::string name = to_string(cls->GetName());
                        // Blueprint editing artefacts, never targets.
                        if (name.starts_with("SKEL_") || name.starts_with("REINST_") || name.starts_with("HOTRELOADED_")) return LoopAction::Continue;
                        index.emplace(cls, rows.size());
                        rows.push_back({name, to_string(cls->GetPathName()), 1, is_actor});
                        return LoopAction::Continue;
                    });
                    std::sort(rows.begin(), rows.end(), [](const ClassRow& a, const ClassRow& b) { return a.name < b.name; });
                },
                std::chrono::milliseconds{10000});
        return rows;
    }
} // namespace RC::Toolkit
