// =============================================================================
// tests/contracts/machine_ir_test.cc
// =============================================================================

#include "tests/test-framework.h"

#include "contracts/machine-ir.h"

using namespace v12;
using namespace v12_test;

TEST(MachineIR, CreateFunction) {
    MachineFunction mf(nullptr);
    EXPECT_EQ(mf.block_count(), 0u);
    MachineBlock* b = mf.NewBlock();
    EXPECT_EQ(mf.block_count(), 1u);
    EXPECT_EQ(b->id, 0u);
}

TEST(MachineIR, AddInstructions) {
    MachineFunction mf(nullptr);
    MachineBlock* b = mf.NewBlock();
    MachineInstruction inst(MachOp::kAdd);
    inst.AddOperand(MachineOperand::VRegOf(0));
    inst.AddOperand(MachineOperand::VRegOf(1));
    b->AddInstruction(std::move(inst));
    EXPECT_EQ(b->instructions.size(), 1u);
}

TEST(MachineIR, CFG) {
    MachineFunction mf(nullptr);
    MachineBlock* b0 = mf.NewBlock();
    MachineBlock* b1 = mf.NewBlock();
    MachineBlock* b2 = mf.NewBlock();
    mf.SetEntryBlock(b0);
    b0->AddSuccessor(b1);
    b0->AddSuccessor(b2);
    EXPECT_EQ(b0->successors.size(), 2u);
    EXPECT_EQ(b1->predecessors.size(), 1u);
    EXPECT_EQ(b2->predecessors.size(), 1u);
}

TEST(MachineIR, VRegs) {
    MachineFunction mf(nullptr);
    VReg r0 = mf.NewVReg(Rep::kInt32);
    VReg r1 = mf.NewVReg(Rep::kFloat64);
    EXPECT_EQ(r0, 0u);
    EXPECT_EQ(r1, 1u);
    EXPECT_EQ(mf.GetRep(r0), Rep::kInt32);
    EXPECT_EQ(mf.GetRep(r1), Rep::kFloat64);
    EXPECT_EQ(mf.vreg_count(), 2u);
}

TEST(MachineIR, StackSlots) {
    MachineFunction mf(nullptr);
    uint32_t s0 = mf.NewStackSlot(8, 8);
    uint32_t s1 = mf.NewStackSlot(16, 16);
    EXPECT_EQ(s0, 0u);
    EXPECT_EQ(s1, 1u);
    EXPECT_EQ(mf.StackSlotOffset(s0), 0u);
    EXPECT_EQ(mf.StackSlotOffset(s1), 16u);
    EXPECT_EQ(mf.frame_size(), 32u);
}

TEST(MachineIR, AllocationResult) {
    AllocationResult ar;
    PhysicalRegister r{RegKind::kGeneral, 0};
    ar.Assign(0, r, Rep::kInt32);
    ar.Spill(1, -8, Rep::kFloat64);
    EXPECT_TRUE(ar.IsInRegister(0));
    EXPECT_TRUE(ar.IsSpilled(1));
    EXPECT_EQ(ar.GetPhysical(0).code, r.code);
    EXPECT_EQ(ar.GetSpillOffset(1), -8);
}

TEST(MachineIR, ReversePostOrder) {
    MachineFunction mf(nullptr);
    MachineBlock* b0 = mf.NewBlock();
    MachineBlock* b1 = mf.NewBlock();
    MachineBlock* b2 = mf.NewBlock();
    MachineBlock* b3 = mf.NewBlock();
    mf.SetEntryBlock(b0);
    b0->AddSuccessor(b1);
    b1->AddSuccessor(b2);
    b1->AddSuccessor(b3);
    b2->AddSuccessor(b3);
    auto rpo = mf.ReversePostOrder();
    EXPECT_EQ(rpo.size(), 4u);
    EXPECT_EQ(rpo[0], b0);
    EXPECT_EQ(rpo[3], b3);
}
