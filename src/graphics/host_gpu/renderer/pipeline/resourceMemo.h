#ifndef KYTY_GRAPHICS_RESOURCE_MEMO_H_
#define KYTY_GRAPHICS_RESOURCE_MEMO_H_

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Libs::Graphics {

// A resource snapshot depends on register inputs AND guest memory. Validate the
// actual reads with their original readers; unchanged pointers alone are insufficient.
class ResourceMemo {
    using Runtime = ShaderRecompiler::IR::SrtRuntime;
    struct Read {
        uint64_t address;
        bool strict;
        std::vector<uint32_t> values;
    };
    struct Capture {
        ResourceMemo& memo;
        const Runtime& source;
    };

    static bool ReadValues(const Runtime& runtime, bool strict, uint64_t address,
                           std::span<uint32_t> values) {
        const auto reader = strict ? runtime.read_specialization_memory : runtime.read_memory;
        if (reader != nullptr) return reader(runtime.userdata, address, values);
        if (strict) return false;
        std::memcpy(values.data(), reinterpret_cast<const void*>(address), values.size_bytes());
        return true;
    }
    template <bool strict>
    static bool CaptureRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
        auto& capture = *static_cast<Capture*>(userdata);
        if (!ReadValues(capture.source, strict, address, values)) return false;
        auto& reads = capture.memo.m_reads;
        if (!reads.empty() && reads.back().strict == strict &&
            reads.back().address + reads.back().values.size() * sizeof(uint32_t) == address) {
            reads.back().values.insert(reads.back().values.end(), values.begin(), values.end());
        } else {
            reads.push_back({address, strict, {values.begin(), values.end()}});
        }
        return true;
    }

public:
    bool Refresh(const ShaderRecompiler::IR::ResourcePlan& plan, const Runtime& runtime,
                 ShaderRecompiler::IR::ResourceSnapshot& snapshot,
                 ShaderRecompiler::IR::ResourceSpecialization& specialization,
                 bool enabled, bool& reused) {
        reused = false;
        if (enabled && m_valid && m_plan == &plan && m_shader_base == runtime.shader_base &&
            RegisterInputsEqual(runtime.user_data) &&
            std::ranges::equal(m_workgroups, runtime.workgroup_counts)) {
            bool equal = true;
            for (const auto& read: m_reads) {
                m_check.resize(read.values.size());
                if (!ReadValues(runtime, read.strict, read.address, m_check) || m_check != read.values) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                // Non-resource SGPRs can vary per draw. They still feed push data,
                // but do not require walking the descriptor graph again.
                snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
                reused = true;
                return true;
            }
        }
        m_valid = false;
        m_reads.clear();
        if (!enabled) {
            return ShaderRecompiler::IR::MaterializeResources(plan, runtime, snapshot, specialization);
        }
        Capture capture{*this, runtime};
        auto observed = runtime;
        observed.userdata = &capture;
        observed.read_memory = CaptureRead<false>;
        observed.read_specialization_memory = runtime.read_specialization_memory != nullptr
                                                ? CaptureRead<true> : nullptr;
        if (!ShaderRecompiler::IR::MaterializeResources(plan, observed, snapshot, specialization)) return false;
        if (m_plan != &plan) {
            m_register_inputs.clear();
            for (const auto& value: plan.value_storage) {
                if (value.GetOpcode() != ShaderRecompiler::IR::ValueOpcode::GetUserData) continue;
                const auto reg = ShaderRecompiler::IR::RegIndex(value.Arg(0).ScalarRegister());
                if (reg >= plan.user_data_base) m_register_inputs.push_back(reg - plan.user_data_base);
            }
            std::ranges::sort(m_register_inputs);
            m_register_inputs.erase(std::unique(m_register_inputs.begin(), m_register_inputs.end()),
                                    m_register_inputs.end());
        }
        m_plan = &plan;
        m_shader_base = runtime.shader_base;
        m_user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
        m_workgroups.assign(runtime.workgroup_counts.begin(), runtime.workgroup_counts.end());
        m_valid = true;
        return true;
    }

private:
    bool RegisterInputsEqual(std::span<const uint32_t> current) const {
        if (current.size() != m_user_data.size()) return false;
        for (const auto index: m_register_inputs) {
            if (index >= current.size() || current[index] != m_user_data[index]) return false;
        }
        return true;
    }
    const ShaderRecompiler::IR::ResourcePlan* m_plan = nullptr;
    uint64_t m_shader_base = 0;
    bool m_valid = false;
    std::vector<uint32_t> m_user_data;
    std::vector<uint32_t> m_workgroups;
    std::vector<uint32_t> m_register_inputs;
    std::vector<Read> m_reads;
    std::vector<uint32_t> m_check;
};

} // namespace Libs::Graphics
#endif
