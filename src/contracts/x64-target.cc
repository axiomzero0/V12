// =============================================================================
// src/contracts/x64-target.cc
// =============================================================================

#include "contracts/x64-target.h"

namespace v12 {

static const char* kGPRNames[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char* kXMMNames[] = {
    "xmm0",  "xmm1",  "xmm2",  "xmm3",
    "xmm4",  "xmm5",  "xmm6",  "xmm7",
    "xmm8",  "xmm9",  "xmm10", "xmm11",
    "xmm12", "xmm13", "xmm14", "xmm15"
};

const char* X64TargetDescription::GeneralRegisterName(PhysicalRegister r) const {
    if (r.kind != RegKind::kGeneral || r.code >= 16) return "?";
    return kGPRNames[r.code];
}

const char* X64TargetDescription::FloatRegisterName(PhysicalRegister r) const {
    if (r.kind != RegKind::kFloat || r.code >= 16) return "?";
    return kXMMNames[r.code];
}

bool X64TargetDescription::IsCalleeSaved(PhysicalRegister r) const {
    for (const auto& c : callee_saved_) {
        if (c.kind == r.kind && c.code == r.code) return true;
    }
    return false;
}

bool X64TargetDescription::IsCallerSaved(PhysicalRegister r) const {
    for (const auto& c : caller_saved_) {
        if (c.kind == r.kind && c.code == r.code) return true;
    }
    return false;
}

// GetHostTargetDescription — returns a singleton X64TargetDescription.
const TargetDescription* GetHostTargetDescription() {
    static X64TargetDescription target;
    return &target;
}

}  // namespace v12
