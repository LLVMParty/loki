#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>
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

struct Options {
  string input_path;
  string output_path;
  string annotation = "loki_virtualize";
  bool use_annotation = true;
  bool all_functions = false;
  bool exported_functions = false;
  vector<string> function_names;
};

static void print_usage() {
  errs() << "Usage: virtualize-uops [options] <input.bc> <output.bc>\n"
         << "Options:\n"
         << "  --annotation <name>       Virtualize functions annotated with <name> "
            "(default: loki_virtualize)\n"
         << "  --no-annotation          Do not use annotation-based selection\n"
         << "  --function <name>        Virtualize a named function; may be repeated\n"
         << "  --exported-functions     Virtualize defined externally visible functions except main\n"
         << "  --all-functions          Virtualize every defined non-intrinsic function\n";
}

static bool parse_options(int argc, char **argv, Options &options) {
  vector<string> positional;
  for (int i = 1; i < argc; ++i) {
    string arg = argv[i];
    if (arg == "--annotation") {
      if (++i >= argc) {
        errs() << "--annotation requires a value\n";
        return false;
      }
      options.annotation = argv[i];
      options.use_annotation = true;
    } else if (arg == "--no-annotation") {
      options.use_annotation = false;
    } else if (arg == "--function") {
      if (++i >= argc) {
        errs() << "--function requires a value\n";
        return false;
      }
      options.function_names.push_back(argv[i]);
    } else if (arg == "--exported-functions") {
      options.exported_functions = true;
    } else if (arg == "--all-functions") {
      options.all_functions = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      exit(0);
    } else if (!arg.empty() && arg[0] == '-') {
      errs() << "Unknown option: " << arg << "\n";
      return false;
    } else {
      positional.push_back(arg);
    }
  }

  if (positional.size() != 2) {
    print_usage();
    return false;
  }
  options.input_path = positional[0];
  options.output_path = positional[1];
  return true;
}

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

static bool is_virtualizable_definition(Function &function) {
  if (function.isDeclaration() || function.isIntrinsic()) {
    return false;
  }

  StringRef name = function.getName();
  return !name.startswith("llvm.") && !name.startswith("loki_vm_") &&
         !name.startswith("vm_alu");
}

static bool has_exported_linkage(Function &function) {
  if (!is_virtualizable_definition(function) || function.getName() == "main") {
    return false;
  }
  return function.hasExternalLinkage() || function.hasExternalWeakLinkage() ||
         function.hasWeakAnyLinkage() || function.hasWeakODRLinkage();
}

static void add_target(vector<Function *> &targets, set<Function *> &seen,
                       Function *function) {
  if (function && is_virtualizable_definition(*function) &&
      seen.insert(function).second) {
    targets.push_back(function);
  }
}

static vector<Function *> select_targets(Module &module, const Options &options) {
  vector<Function *> targets;
  set<Function *> seen;

  if (options.use_annotation) {
    for (Function *function : find_annotated_functions(module, options.annotation)) {
      add_target(targets, seen, function);
    }
  }

  for (const string &name : options.function_names) {
    Function *function = module.getFunction(name);
    if (!function) {
      errs() << "Function not found: " << name << "\n";
      continue;
    }
    add_target(targets, seen, function);
  }

  if (options.exported_functions) {
    for (Function &function : module) {
      if (has_exported_linkage(function)) {
        add_target(targets, seen, &function);
      }
    }
  }

  if (options.all_functions) {
    for (Function &function : module) {
      add_target(targets, seen, &function);
    }
  }

  return targets;
}

static bool is_supported_int(Type *type) {
  return type->isIntegerTy() && type->getIntegerBitWidth() <= 64;
}

static bool is_supported_scalar(Type *type) {
  return is_supported_int(type) || type->isPointerTy();
}

static Value *int_to_i64(IRBuilder<> &builder, Value *value, bool sign_extend) {
  LLVMContext &context = builder.getContext();
  Type *i64 = Type::getInt64Ty(context);
  Type *type = value->getType();

  if (type == i64) {
    return value;
  }
  if (!type->isIntegerTy()) {
    return nullptr;
  }

  unsigned width = type->getIntegerBitWidth();
  if (width < 64) {
    return sign_extend ? builder.CreateSExt(value, i64) : builder.CreateZExt(value, i64);
  }
  if (width > 64) {
    return builder.CreateTrunc(value, i64);
  }
  return nullptr;
}

static Value *to_i64_unsigned(IRBuilder<> &builder, Value *value) {
  Type *type = value->getType();
  if (type->isPointerTy()) {
    return builder.CreatePtrToInt(value, Type::getInt64Ty(builder.getContext()));
  }
  return int_to_i64(builder, value, false);
}

static Value *to_i64_signed(IRBuilder<> &builder, Value *value) {
  return int_to_i64(builder, value, true);
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
  if (type->isPointerTy()) {
    return builder.CreateIntToPtr(value, type);
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
  case Instruction::SDiv:
    return 26;
  case Instruction::SRem:
    return 27;
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

static bool is_signed_binary(unsigned opcode) {
  return opcode == Instruction::AShr || opcode == Instruction::SDiv ||
         opcode == Instruction::SRem;
}

static bool replace_binary(BinaryOperator *binary, FunctionCallee dispatch) {
  int opcode = opcode_for_binary(binary->getOpcode());
  if (opcode < 0 || !is_supported_int(binary->getType())) {
    return false;
  }

  IRBuilder<> builder(binary);
  bool signed_op = is_signed_binary(binary->getOpcode());
  Value *lhs = signed_op ? to_i64_signed(builder, binary->getOperand(0))
                         : to_i64_unsigned(builder, binary->getOperand(0));
  Value *rhs = to_i64_unsigned(builder, binary->getOperand(1));
  if (binary->getOpcode() == Instruction::SDiv || binary->getOpcode() == Instruction::SRem) {
    rhs = to_i64_signed(builder, binary->getOperand(1));
  }
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

static bool is_signed_icmp(ICmpInst::Predicate predicate) {
  return predicate == ICmpInst::ICMP_SLT || predicate == ICmpInst::ICMP_SLE ||
         predicate == ICmpInst::ICMP_SGT || predicate == ICmpInst::ICMP_SGE;
}

static bool replace_icmp(ICmpInst *icmp, FunctionCallee dispatch) {
  int opcode = opcode_for_icmp(icmp->getPredicate());
  if (opcode < 0) {
    return false;
  }

  Type *operand_type = icmp->getOperand(0)->getType();
  if (!is_supported_scalar(operand_type) ||
      operand_type != icmp->getOperand(1)->getType()) {
    return false;
  }
  if (operand_type->isPointerTy() && is_signed_icmp(icmp->getPredicate())) {
    return false;
  }

  IRBuilder<> builder(icmp);
  bool signed_compare = is_signed_icmp(icmp->getPredicate());
  Value *lhs = signed_compare ? to_i64_signed(builder, icmp->getOperand(0))
                              : to_i64_unsigned(builder, icmp->getOperand(0));
  Value *rhs = signed_compare ? to_i64_signed(builder, icmp->getOperand(1))
                              : to_i64_unsigned(builder, icmp->getOperand(1));
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
  if (!is_supported_scalar(select->getType())) {
    return false;
  }
  if (!select->getCondition()->getType()->isIntegerTy(1)) {
    return false;
  }

  IRBuilder<> builder(select);
  Value *true_value = to_i64_unsigned(builder, select->getTrueValue());
  Value *false_value = to_i64_unsigned(builder, select->getFalseValue());
  Value *condition = to_i64_unsigned(builder, select->getCondition());
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
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 1;
  }

  LLVMContext context;
  SMDiagnostic error;
  auto module = parseIRFile(options.input_path, error, context);
  if (!module) {
    error.print("virtualize-uops", errs());
    return 1;
  }

  auto targets = select_targets(*module, options);
  if (targets.empty()) {
    errs() << "No target functions selected. Use annotations, --function, "
              "--exported-functions, or --all-functions.\n";
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
  raw_fd_ostream output(options.output_path, ec, sys::fs::F_None);
  if (ec) {
    errs() << "Could not open output: " << ec.message() << "\n";
    return 1;
  }
  WriteBitcodeToFile(*module, output);
  return 0;
}
