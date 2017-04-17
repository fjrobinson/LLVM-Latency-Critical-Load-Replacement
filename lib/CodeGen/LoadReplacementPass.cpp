#include "LoadReplacementPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/IR/ModuleSlotTracker.h"

#include <cstdio>

using namespace llvm;

// namespace llvm {
//   class BBPaths {
//   private:
//     //std::vector<std::vector<
//   };
// }

char LoadReplacementPass::ID;

LoadReplacementPass::LoadReplacementPass() : MachineFunctionPass(ID) {

}

StringRef LoadReplacementPass::getPassName() const {
  return "Load Replacement";
}

void LoadReplacementPass::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
}

bool LoadReplacementPass::runOnMachineFunction(MachineFunction& mf) {
  auto os = llvm::make_unique<raw_fd_ostream>(1, false);
  const unsigned indent = 4;

  ModuleSlotTracker MST(mf.getFunction()->getParent());
  MST.incorporateFunction(*mf.getFunction());

  for (MachineBasicBlock &MBB : mf) {
    // Original basic block printer
    //MBB.print(*os, MST, nullptr);

    os->indent(0*indent);

    // Print basic block information
    *os << "BB#" << MBB.getNumber() << ": " << MBB.getFullName();

    // Print predecessor information
    if (!MBB.pred_empty()){
      *os << " PREDESSORS:";
      for (MachineBasicBlock::const_pred_iterator PI = MBB.pred_begin(), E = MBB.pred_end(); PI != E; ++PI) {
        *os << " BB#" << (*PI)->getNumber();
      }
    }
    *os << " {\n";

    // Iterate over instructions
    for (MachineBasicBlock::iterator MII = MBB.begin(), MIE = MBB.end(); MII != MIE; ) {
      MachineInstr *MI = &*MII;
      ++MII;

      os->indent(1*indent);

      // Print machine instruction
      MI->print(*os, MST);

      if (MI->mayLoad()){
        // This instruction probably loads from memory. It most likely loads
        // into any register that is marked as "defined" in this instruction.
        // This means we can track that register and watch for it being used in
        // the control flow of the instructions and basic blocks in a function.

        // Iterate over operands looking for all register operands marked as
        // "defined"
        for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
          MachineOperand& MIOP = MI->getOperand(i);
          if (MIOP.isReg() && MIOP.isDef()) {
            // TODO: Track this register.
            os->indent(2*indent);
            *os << "DEF:";
            MI->getOperand(i).print(*os, MST, mf.getSubtarget().getRegisterInfo(), mf.getTarget().getIntrinsicInfo());
            *os << "\n";
          }
        }
      }
      else {
        // This is not a load instruction but it may be a use. Iterate through
        // the operands, looking for register usages that have not occured since
        // the last load instruction on that register.
        for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
          MachineOperand& MIOP = MI->getOperand(i);

          // Check if we are using a register for the first time.
          // TODO: Check that this check for use is sufficient.
          if (MIOP.isReg() && MIOP.isImplicit() && !MIOP.isDef()) {
            // TODO: Mark register as used
            os->indent(2*indent);
            *os << "USE: ";
            MI->getOperand(i).print(*os, MST, mf.getSubtarget().getRegisterInfo(), mf.getTarget().getIntrinsicInfo());
            *os << "\n";
          }
        }
      }
    }

    *os << "}";

    // We may want to explore probabilistic branching for determining distance
    // later. For now we just print the successors and their branching
    // probabilities.
    if (!MBB.succ_empty()){
      MachineBranchProbabilityInfo MBPI;

      *os << " SUCCESSORS:";
      for (MachineBasicBlock::const_succ_iterator SI = MBB.succ_begin(), E = MBB.succ_end(); SI != E; ++SI) {
        *os << " BB#" << (*SI)->getNumber();
        if (MBB.hasSuccessorProbabilities()){
          *os << '(' << MBPI.getEdgeProbability(&MBB, SI) << ')';
        }
      }
    }
    *os << "\n\n";
  }

  return false;
}

/* class MemoryLoadInstr { */
/*  private: */
/*   MachineFunction* machine_function; */
/*   MachineInstr* instr; */
/*   std::vector<UseInstr> users; */

/*  public: */
/*   MachineInstr* getInstr() { return instr; } */
/*   std::vector<UseInstr> getUsers(); */
/* } */


/* class UseInstr { */
/*  private: */
/*   MachineFunction* machine_function; */
/*   MachineInstr* instr; */


/*  public: */

/* } */
