#ifndef LLVM_LIB_CODEGEN_LOADREPLACEMENTPASS_H
#define LLVM_LIB_CODEGEN_LOADREPLACEMENTPASS_H

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
  class MachineFunction;

  class LoadReplacementPass: public MachineFunctionPass {
  public:
    LoadReplacementPass();
    virtual bool runOnMachineFunction(MachineFunction& mf) override;
  };
}
#endif
