#ifndef LLVM_LIB_CODEGEN_LOADREPLACEMENTPASS_H
#define LLVM_LIB_CODEGEN_LOADREPLACEMENTPASS_H

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
  class MachineFunction;
  class AnalysisUsage;

  class LoadReplacementPass: public MachineFunctionPass {
  private:
    static char ID;

  public:
    LoadReplacementPass();
    StringRef getPassName() const override;

    void getAnalysisUsage(AnalysisUsage &AU) const override;
    virtual bool runOnMachineFunction(MachineFunction& mf) override;
  };
}
#endif
