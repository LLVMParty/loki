#include <set>
#include <string>
#include <vector>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace std;

static Function *annotation_function_operand(Value *value) {
  if (!value) {
    return nullptr;
  }

  value = value->stripPointerCasts();
  if (auto *function = dyn_cast<Function>(value)) {
    return function;
  }

  if (auto *constant_expr = dyn_cast<ConstantExpr>(value)) {
    if (constant_expr->isCast()) {
      return annotation_function_operand(constant_expr->getOperand(0));
    }
  }

  return nullptr;
}

static string annotation_string_operand(Value *value) {
  if (!value) {
    return "";
  }

  value = value->stripPointerCasts();
  if (auto *constant_expr = dyn_cast<ConstantExpr>(value)) {
    if (constant_expr->getOpcode() == Instruction::GetElementPtr &&
        constant_expr->getNumOperands() > 0) {
      value = constant_expr->getOperand(0)->stripPointerCasts();
    }
  }

  auto *global = dyn_cast<GlobalVariable>(value);
  if (!global || !global->hasInitializer()) {
    return "";
  }

  auto *array = dyn_cast<ConstantDataArray>(global->getInitializer());
  if (!array || !array->isCString()) {
    return "";
  }

  return array->getAsCString().str();
}

static vector<Function *> find_annotated_functions(Module &module,
                                                   const string &annotation) {
  vector<Function *> functions;
  auto *annotations = module.getGlobalVariable("llvm.global.annotations");
  if (!annotations || !annotations->hasInitializer()) {
    return functions;
  }

  auto *annotation_array = dyn_cast<ConstantArray>(annotations->getInitializer());
  if (!annotation_array) {
    return functions;
  }

  set<Function *> seen;
  for (auto &operand : annotation_array->operands()) {
    auto *annotation_struct = dyn_cast<ConstantStruct>(operand.get());
    if (!annotation_struct || annotation_struct->getNumOperands() < 2) {
      continue;
    }

    Function *function = annotation_function_operand(annotation_struct->getOperand(0));
    string text = annotation_string_operand(annotation_struct->getOperand(1));
    if (function && text == annotation && seen.insert(function).second) {
      functions.push_back(function);
    }
  }

  return functions;
}

static bool is_supported_int(Type *type) {
  return type->isIntegerTy() && type->getIntegerBitWidth() <= 64;
}

static Value *to_i64(IRBuilder<> &builder, Value *value) {
  LLVMContext &context = builder.getContext();
  Type *i64 = Type::getInt64Ty(context);
  Type *type = value->getType();

  if (type == i64) {
    return value;
  }
  if (type->isIntegerTy()) {
    unsigned width = type->getIntegerBitWidth();
    if (width < 64) {
      return builder.CreateZExt(value, i64);
    }
    if (width > 64) {
      return builder.CreateTrunc(value, i64);
    }
  }
  if (type->isPointerTy()) {
    return builder.CreatePtrToInt(value, i64);
  }

  return nullptr;
}

static Value *from_i64(IRBuilder<> &builder, Value *value, Type *type) {
  if (type == value->getType()) {
    return value;
  }
  if (type->isIntegerTy()) {
    unsigned width = type->getIntegerBitWidth();
    if (width < 64) {
      return builder.CreateTrunc(value, type);
    }
    if (width > 64) {
      return builder.CreateZExt(value, type);
    }
  }
  return nullptr;
}

static int opcode_for_binary(unsigned opcode) {
  switch (opcode) {
  case Instruction::Add:
    return 0;
  case Instruction::Sub:
    return 1;
  case Instruction::Xor:
    return 2;
  case Instruction::And:
    return 3;
  case Instruction::Or:
    return 4;
  case Instruction::Mul:
    return 5;
  case Instruction::LShr:
    return 6;
  case Instruction::Shl:
    return 7;
  case Instruction::UDiv:
    return 9;
  case Instruction::URem:
    return 10;
  case Instruction::AShr:
    return 25;
  default:
    return -1;
  }
}

static FunctionCallee get_dispatch(Module &module) {
  LLVMContext &context = module.getContext();
  Type *i64 = Type::getInt64Ty(context);
  auto *func_type = FunctionType::get(i64, {i64, i64, i64, i64}, false);
  return module.getOrInsertFunction("loki_vm_enter", func_type);
}

static bool replace_binary(BinaryOperator *binary, FunctionCallee dispatch) {
  int opcode = opcode_for_binary(binary->getOpcode());
  if (opcode < 0 || !is_supported_int(binary->getType())) {
    return false;
  }

  IRBuilder<> builder(binary);
  Value *lhs = to_i64(builder, binary->getOperand(0));
  Value *rhs = to_i64(builder, binary->getOperand(1));
  if (!lhs || !rhs) {
    return false;
  }

  Value *op = builder.getInt64(opcode);
  Value *zero = builder.getInt64(0);
  Value *call = builder.CreateCall(dispatch, {op, lhs, rhs, zero}, "loki.uop");
  Value *replacement = from_i64(builder, call, binary->getType());
  if (!replacement) {
    return false;
  }

  binary->replaceAllUsesWith(replacement);
  binary->eraseFromParent();
  return true;
}

static int opcode_for_icmp(ICmpInst::Predicate predicate) {
  switch (predicate) {
  case ICmpInst::ICMP_EQ:
    return 11;
  case ICmpInst::ICMP_NE:
    return 16;
  case ICmpInst::ICMP_ULT:
    return 17;
  case ICmpInst::ICMP_ULE:
    return 18;
  case ICmpInst::ICMP_UGT:
    return 19;
  case ICmpInst::ICMP_UGE:
    return 20;
  case ICmpInst::ICMP_SLT:
    return 21;
  case ICmpInst::ICMP_SLE:
    return 22;
  case ICmpInst::ICMP_SGT:
    return 23;
  case ICmpInst::ICMP_SGE:
    return 24;
  default:
    return -1;
  }
}

static bool replace_icmp(ICmpInst *icmp, FunctionCallee dispatch) {
  int opcode = opcode_for_icmp(icmp->getPredicate());
  if (opcode < 0) {
    return false;
  }
  if (!is_supported_int(icmp->getOperand(0)->getType()) ||
      !is_supported_int(icmp->getOperand(1)->getType())) {
    return false;
  }

  IRBuilder<> builder(icmp);
  Value *lhs = to_i64(builder, icmp->getOperand(0));
  Value *rhs = to_i64(builder, icmp->getOperand(1));
  if (!lhs || !rhs) {
    return false;
  }

  Value *call = builder.CreateCall(dispatch,
                                   {builder.getInt64(opcode), lhs, rhs, builder.getInt64(0)},
                                   "loki.icmp");
  Value *replacement = builder.CreateTrunc(call, icmp->getType());
  icmp->replaceAllUsesWith(replacement);
  icmp->eraseFromParent();
  return true;
}

static bool replace_select(SelectInst *select, FunctionCallee dispatch) {
  if (!is_supported_int(select->getType())) {
    return false;
  }

  IRBuilder<> builder(select);
  Value *true_value = to_i64(builder, select->getTrueValue());
  Value *false_value = to_i64(builder, select->getFalseValue());
  Value *condition = to_i64(builder, select->getCondition());
  if (!true_value || !false_value || !condition) {
    return false;
  }

  Value *call = builder.CreateCall(
      dispatch, {builder.getInt64(13), true_value, false_value, condition}, "loki.select");
  Value *replacement = from_i64(builder, call, select->getType());
  if (!replacement) {
    return false;
  }

  select->replaceAllUsesWith(replacement);
  select->eraseFromParent();
  return true;
}

static size_t virtualize_function(Function &function, FunctionCallee dispatch) {
  vector<Instruction *> instructions;
  for (auto &instruction : llvm::instructions(function)) {
    instructions.push_back(&instruction);
  }

  size_t replaced = 0;
  for (Instruction *instruction : instructions) {
    if (!instruction->getParent()) {
      continue;
    }
    if (auto *binary = dyn_cast<BinaryOperator>(instruction)) {
      replaced += replace_binary(binary, dispatch) ? 1 : 0;
    } else if (auto *icmp = dyn_cast<ICmpInst>(instruction)) {
      replaced += replace_icmp(icmp, dispatch) ? 1 : 0;
    } else if (auto *select = dyn_cast<SelectInst>(instruction)) {
      replaced += replace_select(select, dispatch) ? 1 : 0;
    }
  }

  return replaced;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    errs() << "Usage: virtualize-uops <input.bc> <output.bc>\n";
    return 1;
  }

  LLVMContext context;
  SMDiagnostic error;
  auto module = parseIRFile(argv[1], error, context);
  if (!module) {
    error.print("virtualize-uops", errs());
    return 1;
  }

  auto targets = find_annotated_functions(*module, "loki_virtualize");
  if (targets.empty()) {
    errs() << "No loki_virtualize annotations found.\n";
    return 1;
  }

  FunctionCallee dispatch = get_dispatch(*module);
  size_t total_replaced = 0;
  for (Function *target : targets) {
    size_t replaced = virtualize_function(*target, dispatch);
    total_replaced += replaced;
    errs() << "virtualized " << replaced << " uops in " << target->getName() << "\n";
  }

  if (total_replaced == 0) {
    errs() << "No supported instructions were virtualized.\n";
    return 1;
  }

  error_code ec;
  raw_fd_ostream output(argv[2], ec, sys::fs::F_None);
  if (ec) {
    errs() << "Could not open output: " << ec.message() << "\n";
    return 1;
  }
  WriteBitcodeToFile(*module, output);
  return 0;
}
