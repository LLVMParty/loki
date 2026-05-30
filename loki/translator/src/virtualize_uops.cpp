#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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

struct UopConfig {
  map<string, uint64_t> opcodes;
  uint64_t encode_op_xor = 0;
  uint64_t encode_a_xor = 0;
  uint64_t encode_b_xor = 0;
  uint64_t encode_c_xor = 0;
};

struct Options {
  string input_path;
  string output_path;
  string annotation = "loki_virtualize";
  string op_map_path;
  bool use_annotation = true;
  bool all_functions = false;
  bool exported_functions = false;
  vector<string> function_names;
  UopConfig uop_config;
};

static void set_default_uop_config(UopConfig &config) {
  config.opcodes["add"] = 0;
  config.opcodes["sub"] = 1;
  config.opcodes["xor"] = 2;
  config.opcodes["and"] = 3;
  config.opcodes["or"] = 4;
  config.opcodes["mul"] = 5;
  config.opcodes["lshr"] = 6;
  config.opcodes["shl"] = 7;
  config.opcodes["not"] = 8;
  config.opcodes["udiv"] = 9;
  config.opcodes["urem"] = 10;
  config.opcodes["eq"] = 11;
  config.opcodes["iszero"] = 12;
  config.opcodes["select"] = 13;
  config.opcodes["addc"] = 14;
  config.opcodes["xorc"] = 15;
  config.opcodes["ne"] = 16;
  config.opcodes["ult"] = 17;
  config.opcodes["ule"] = 18;
  config.opcodes["ugt"] = 19;
  config.opcodes["uge"] = 20;
  config.opcodes["slt"] = 21;
  config.opcodes["sle"] = 22;
  config.opcodes["sgt"] = 23;
  config.opcodes["sge"] = 24;
  config.opcodes["ashr"] = 25;
  config.opcodes["sdiv"] = 26;
  config.opcodes["srem"] = 27;
}

static string trim(const string &value) {
  const char *spaces = " \t\r\n";
  size_t begin = value.find_first_not_of(spaces);
  if (begin == string::npos) {
    return "";
  }
  size_t end = value.find_last_not_of(spaces);
  return value.substr(begin, end - begin + 1);
}

static bool parse_u64(const string &text, uint64_t &value) {
  char *end = nullptr;
  value = strtoull(text.c_str(), &end, 0);
  return end && *end == '\0';
}

static bool apply_uop_map_line(UopConfig &config, const string &key,
                               uint64_t value) {
  if (key == "encode_op_xor") {
    config.encode_op_xor = value;
    return true;
  }
  if (key == "encode_a_xor") {
    config.encode_a_xor = value;
    return true;
  }
  if (key == "encode_b_xor") {
    config.encode_b_xor = value;
    return true;
  }
  if (key == "encode_c_xor") {
    config.encode_c_xor = value;
    return true;
  }
  if (config.opcodes.count(key)) {
    config.opcodes[key] = value;
    return true;
  }
  return false;
}

static bool load_uop_map(const string &path, UopConfig &config) {
  set_default_uop_config(config);
  if (path.empty()) {
    return true;
  }

  ifstream input(path);
  if (!input) {
    errs() << "Could not open uop map: " << path << "\n";
    return false;
  }

  string raw_line;
  unsigned line_number = 0;
  while (getline(input, raw_line)) {
    ++line_number;
    string line = raw_line.substr(0, raw_line.find('#'));
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    istringstream stream(line);
    string key;
    string value_text;
    string trailing;
    stream >> key >> value_text >> trailing;
    if (key.empty() || value_text.empty() || !trailing.empty()) {
      errs() << "Invalid uop map line " << line_number << " in " << path << "\n";
      return false;
    }

    uint64_t value = 0;
    if (!parse_u64(value_text, value)) {
      errs() << "Invalid uop map value on line " << line_number << " in " << path << "\n";
      return false;
    }
    if (!apply_uop_map_line(config, key, value)) {
      errs() << "Unknown uop map key '" << key << "' on line " << line_number << " in " << path << "\n";
      return false;
    }
  }

  return true;
}

static uint64_t opcode_value(const UopConfig &config, const string &name) {
  auto it = config.opcodes.find(name);
  if (it == config.opcodes.end()) {
    errs() << "Missing uop opcode for " << name << "\n";
    exit(1);
  }
  return it->second;
}

static void print_usage() {
  errs() << "Usage: virtualize-uops [options] <input.bc> <output.bc>\n"
         << "Options:\n"
         << "  --annotation <name>       Virtualize functions annotated with <name> "
            "(default: loki_virtualize)\n"
         << "  --no-annotation          Do not use annotation-based selection\n"
         << "  --function <name>        Virtualize a named function; may be repeated\n"
         << "  --exported-functions     Virtualize defined externally visible functions except main\n"
         << "  --all-functions          Virtualize every defined non-intrinsic function\n"
         << "  --op-map <path>          Uop opcode/encoding map generated by generate_uop_dispatch.py\n";
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
    } else if (arg == "--op-map") {
      if (++i >= argc) {
        errs() << "--op-map requires a value\n";
        return false;
      }
      options.op_map_path = argv[i];
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

static Value *encode_i64(IRBuilder<> &builder, Value *value, uint64_t xor_key) {
  if (xor_key == 0) {
    return value;
  }
  return builder.CreateXor(value, ConstantInt::get(value->getType(), xor_key),
                           "loki.enc");
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

static const char *operation_for_binary(unsigned opcode) {
  switch (opcode) {
  case Instruction::Add:
    return "add";
  case Instruction::Sub:
    return "sub";
  case Instruction::Xor:
    return "xor";
  case Instruction::And:
    return "and";
  case Instruction::Or:
    return "or";
  case Instruction::Mul:
    return "mul";
  case Instruction::LShr:
    return "lshr";
  case Instruction::Shl:
    return "shl";
  case Instruction::UDiv:
    return "udiv";
  case Instruction::URem:
    return "urem";
  case Instruction::AShr:
    return "ashr";
  case Instruction::SDiv:
    return "sdiv";
  case Instruction::SRem:
    return "srem";
  default:
    return nullptr;
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

static bool replace_binary(BinaryOperator *binary, FunctionCallee dispatch,
                           const UopConfig &config) {
  const char *operation = operation_for_binary(binary->getOpcode());
  if (!operation || !is_supported_int(binary->getType())) {
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

  uint64_t opcode = opcode_value(config, operation);
  Value *op = builder.getInt64(opcode ^ config.encode_op_xor);
  Value *encoded_lhs = encode_i64(builder, lhs, config.encode_a_xor);
  Value *encoded_rhs = encode_i64(builder, rhs, config.encode_b_xor);
  Value *encoded_zero = builder.getInt64(config.encode_c_xor);
  Value *call = builder.CreateCall(dispatch, {op, encoded_lhs, encoded_rhs, encoded_zero},
                                   "loki.uop");
  Value *replacement = from_i64(builder, call, binary->getType());
  if (!replacement) {
    return false;
  }

  binary->replaceAllUsesWith(replacement);
  binary->eraseFromParent();
  return true;
}

static const char *operation_for_icmp(ICmpInst::Predicate predicate) {
  switch (predicate) {
  case ICmpInst::ICMP_EQ:
    return "eq";
  case ICmpInst::ICMP_NE:
    return "ne";
  case ICmpInst::ICMP_ULT:
    return "ult";
  case ICmpInst::ICMP_ULE:
    return "ule";
  case ICmpInst::ICMP_UGT:
    return "ugt";
  case ICmpInst::ICMP_UGE:
    return "uge";
  case ICmpInst::ICMP_SLT:
    return "slt";
  case ICmpInst::ICMP_SLE:
    return "sle";
  case ICmpInst::ICMP_SGT:
    return "sgt";
  case ICmpInst::ICMP_SGE:
    return "sge";
  default:
    return nullptr;
  }
}

static bool is_signed_icmp(ICmpInst::Predicate predicate) {
  return predicate == ICmpInst::ICMP_SLT || predicate == ICmpInst::ICMP_SLE ||
         predicate == ICmpInst::ICMP_SGT || predicate == ICmpInst::ICMP_SGE;
}

static bool replace_icmp(ICmpInst *icmp, FunctionCallee dispatch,
                         const UopConfig &config) {
  const char *operation = operation_for_icmp(icmp->getPredicate());
  if (!operation) {
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

  uint64_t opcode = opcode_value(config, operation);
  Value *call = builder.CreateCall(
      dispatch,
      {builder.getInt64(opcode ^ config.encode_op_xor),
       encode_i64(builder, lhs, config.encode_a_xor),
       encode_i64(builder, rhs, config.encode_b_xor),
       builder.getInt64(config.encode_c_xor)},
      "loki.icmp");
  Value *replacement = builder.CreateTrunc(call, icmp->getType());
  icmp->replaceAllUsesWith(replacement);
  icmp->eraseFromParent();
  return true;
}

static bool replace_select(SelectInst *select, FunctionCallee dispatch,
                           const UopConfig &config) {
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

  uint64_t opcode = opcode_value(config, "select");
  Value *call = builder.CreateCall(
      dispatch,
      {builder.getInt64(opcode ^ config.encode_op_xor),
       encode_i64(builder, true_value, config.encode_a_xor),
       encode_i64(builder, false_value, config.encode_b_xor),
       encode_i64(builder, condition, config.encode_c_xor)},
      "loki.select");
  Value *replacement = from_i64(builder, call, select->getType());
  if (!replacement) {
    return false;
  }

  select->replaceAllUsesWith(replacement);
  select->eraseFromParent();
  return true;
}

static size_t virtualize_function(Function &function, FunctionCallee dispatch,
                                  const UopConfig &config) {
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
      replaced += replace_binary(binary, dispatch, config) ? 1 : 0;
    } else if (auto *icmp = dyn_cast<ICmpInst>(instruction)) {
      replaced += replace_icmp(icmp, dispatch, config) ? 1 : 0;
    } else if (auto *select = dyn_cast<SelectInst>(instruction)) {
      replaced += replace_select(select, dispatch, config) ? 1 : 0;
    }
  }

  if (replaced > 0) {
    function.removeFnAttr(Attribute::ReadNone);
    function.removeFnAttr(Attribute::ReadOnly);
    function.removeFnAttr(Attribute::ArgMemOnly);
    function.removeFnAttr(Attribute::InaccessibleMemOnly);
    function.removeFnAttr(Attribute::InaccessibleMemOrArgMemOnly);
  }

  return replaced;
}

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 1;
  }
  if (!load_uop_map(options.op_map_path, options.uop_config)) {
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
    size_t replaced = virtualize_function(*target, dispatch, options.uop_config);
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
