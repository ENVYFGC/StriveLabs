#pragma once

#include "combo_log.h"
#include "perf_log.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace object_scan {
struct Want
{
    const wchar_t* name;
    std::vector<RC::Unreal::UObject*>* out;
};

inline void pass(const Want* wants, size_t count)
{
    using namespace RC::Unreal;
    if (count == 0) return;
    if (count > 32)
    {
        pass(wants, 32);
        pass(wants + 32, count - 32);
        return;
    }
    const bool timed = perf::on;
    const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::vector<FName> names;
    for (size_t i = 0; i < count; i++) names.emplace_back(wants[i].name);
    std::unordered_map<UClass*, uint32_t> known;
    UClass* last = nullptr;
    uint32_t last_mask = 0;
    size_t looked = 0, found = 0;
    UObjectGlobals::ForEachUObject([&](UObject* o, int32_t, int32_t) {
        looked++;
        UClass* c = o ? o->GetClassPrivate() : nullptr;
        if (!c) return RC::LoopAction::Continue;
        if (c != last)
        {
            auto it = known.find(c);
            if (it == known.end())
            {
                uint32_t mask = 0;
                for (UStruct* k = c; k; k = k->GetSuperStruct())
                    for (size_t i = 0; i < count; i++)
                        if (k->GetNamePrivate().Equals(names[i])) mask |= 1u << i;
                it = known.emplace(c, mask).first;
            }
            last = c;
            last_mask = it->second;
        }
        if (!last_mask || o->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) || o->IsA<UClass>()) return RC::LoopAction::Continue;
        for (size_t i = 0; i < count; i++)
            if (last_mask & (1u << i))
            {
                wants[i].out->push_back(o);
                found++;
            }
        return RC::LoopAction::Continue;
    });
    if (timed)
    {
        char b[200];
        snprintf(b, sizeof(b), "perf: object scan for %ls%s: %zu of %zu objects, %.2f ms", wants[0].name, count > 1 ? " and others" : "", found, looked,
                 perf::since(t0));
        COMBO_LOG(b);
    }
}

struct Batch;
inline Batch*& active()
{
    static thread_local Batch* batch = nullptr;
    return batch;
}

struct Batch
{
    std::map<std::wstring, std::vector<RC::Unreal::UObject*>> found;
    Batch* before;
    bool looked = false;

    explicit Batch(std::initializer_list<const wchar_t*> names) : before(active())
    {
        for (const wchar_t* n : names) found[n];
        active() = this;
    }
    void look()
    {
        if (looked) return;
        looked = true;
        std::vector<Want> wants;
        for (auto& [name, list] : found) wants.push_back({name.c_str(), &list});
        pass(wants.data(), wants.size());
    }
    ~Batch() { active() = before; }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
};

inline void allOf(const wchar_t* name, std::vector<RC::Unreal::UObject*>& out)
{
    if (Batch* b = active())
    {
        const auto it = b->found.find(name);
        if (it != b->found.end())
        {
            b->look();
            out.insert(out.end(), it->second.begin(), it->second.end());
            return;
        }
    }
    const Want one{name, &out};
    pass(&one, 1);
}

inline void allOf(std::initializer_list<Want> wants) { pass(wants.begin(), wants.size()); }

inline void allOf(const std::vector<const wchar_t*>& names, std::vector<RC::Unreal::UObject*>& out)
{
    std::vector<std::vector<RC::Unreal::UObject*>> each(names.size());
    std::vector<Want> wants;
    for (size_t i = 0; i < names.size(); i++) wants.push_back({names[i], &each[i]});
    pass(wants.data(), wants.size());
    for (const auto& v : each) out.insert(out.end(), v.begin(), v.end());
}
}
