#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
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
  string batch_cpp_path;
  bool use_annotation = true;
  bool enable_batching = true;
  bool all_functions = false;
  bool exported_functions = false;
  vector<string> function_names;
  UopConfig uop_config;
  unsigned max_batch_nodes = 12;
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
         << "  --op-map <path>          Uop opcode/encoding map generated by generate_uop_dispatch.py\n"
         << "  --batch-cpp <path>       Emit generated target-specific batch semantics for uop_dispatch.cpp\n"
         << "  --no-batch              Disable expression batching\n"
         << "  --max-batch-nodes <n>    Maximum supported instructions per batch (default: 12)\n";
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
    } else if (arg == "--batch-cpp") {
      if (++i >= argc) {
        errs() << "--batch-cpp requires a value\n";
        return false;
      }
      options.batch_cpp_path = argv[i];
    } else if (arg == "--no-batch") {
      options.enable_batching = false;
    } else if (arg == "--max-batch-nodes") {
      if (++i >= argc) {
        errs() << "--max-batch-nodes requires a value\n";
        return false;
      }
      uint64_t parsed = 0;
      if (!parse_u64(argv[i], parsed) || parsed < 2 || parsed > 64) {
        errs() << "--max-batch-nodes must be in [2, 64]\n";
        return false;
      }
      options.max_batch_nodes = static_cast<unsigned>(parsed);
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

struct BatchCandidate {
  Instruction *root = nullptr;
  vector<Instruction *> nodes;
  vector<Value *> inputs;
  map<Value *, unsigned> input_index;
  map<Instruction *, string> names;
  set<Instruction *> visiting;
  string key;
  unsigned max_nodes = 8;
};

struct BatchState {
  string cpp;
  map<string, uint64_t> opcode_by_key;
  map<string, string> name_by_key;
  set<uint64_t> used_opcodes;
  size_t emitted = 0;

  explicit BatchState(const UopConfig &config) {
    for (auto &entry : config.opcodes) {
      used_opcodes.insert(entry.second);
    }
  }
};

struct FunctionStats {
  size_t virtualized_uops = 0;
  size_t vm_entries = 0;
  size_t batches = 0;
};

static unsigned value_width(Value *value) {
  Type *type = value->getType();
  if (type->isPointerTy()) {
    return 64;
  }
  if (type->isIntegerTy()) {
    return type->getIntegerBitWidth();
  }
  return 0;
}

static bool is_supported_leaf(Value *value) {
  if (auto *constant = dyn_cast<ConstantInt>(value)) {
    return constant->getBitWidth() <= 64;
  }
  if (isa<ConstantPointerNull>(value)) {
    return true;
  }
  return is_supported_scalar(value->getType());
}

static bool is_batch_supported_instruction(Instruction *instruction) {
  if (auto *binary = dyn_cast<BinaryOperator>(instruction)) {
    return operation_for_binary(binary->getOpcode()) && is_supported_int(binary->getType());
  }
  if (auto *icmp = dyn_cast<ICmpInst>(instruction)) {
    Type *operand_type = icmp->getOperand(0)->getType();
    return operation_for_icmp(icmp->getPredicate()) && is_supported_scalar(operand_type) &&
           operand_type == icmp->getOperand(1)->getType() &&
           !(operand_type->isPointerTy() && is_signed_icmp(icmp->getPredicate()));
  }
  if (auto *select = dyn_cast<SelectInst>(instruction)) {
    return is_supported_scalar(select->getType()) &&
           select->getCondition()->getType()->isIntegerTy(1);
  }
  return false;
}

static bool add_batch_leaf(Value *value, BatchCandidate &candidate) {
  if (!is_supported_leaf(value)) {
    return false;
  }
  if (isa<ConstantInt>(value) || isa<ConstantPointerNull>(value)) {
    return true;
  }

  if (candidate.input_index.count(value)) {
    return true;
  }
  if (candidate.inputs.size() >= 3) {
    return false;
  }
  unsigned index = static_cast<unsigned>(candidate.inputs.size());
  candidate.inputs.push_back(value);
  candidate.input_index[value] = index;
  return true;
}

static bool collect_batch_value(Value *value, BasicBlock *block,
                                const set<Instruction *> &consumed,
                                BatchCandidate &candidate, bool is_root);

static bool collect_batch_instruction(Instruction *instruction, BasicBlock *block,
                                      const set<Instruction *> &consumed,
                                      BatchCandidate &candidate, bool is_root) {
  if (!instruction || instruction->getParent() != block || consumed.count(instruction) ||
      !is_batch_supported_instruction(instruction)) {
    return false;
  }
  if (!is_root && !instruction->hasOneUse()) {
    return false;
  }
  if (candidate.nodes.size() >= candidate.max_nodes || candidate.visiting.count(instruction)) {
    return false;
  }

  candidate.visiting.insert(instruction);
  for (Use &operand : instruction->operands()) {
    if (!collect_batch_value(operand.get(), block, consumed, candidate, false)) {
      candidate.visiting.erase(instruction);
      return false;
    }
  }
  candidate.visiting.erase(instruction);

  candidate.names[instruction] = "";
  candidate.nodes.push_back(instruction);
  return true;
}

static bool collect_batch_value(Value *value, BasicBlock *block,
                                const set<Instruction *> &consumed,
                                BatchCandidate &candidate, bool is_root) {
  if (auto *instruction = dyn_cast<Instruction>(value)) {
    if (instruction->getParent() == block && !consumed.count(instruction) &&
        is_batch_supported_instruction(instruction) && (is_root || instruction->hasOneUse())) {
      BatchCandidate snapshot = candidate;
      if (collect_batch_instruction(instruction, block, consumed, candidate, is_root)) {
        return true;
      }
      candidate = snapshot;
    }
  }
  return add_batch_leaf(value, candidate);
}

static string hex_u64(uint64_t value) {
  stringstream stream;
  stream << "0x" << hex << setw(16) << setfill('0') << value << "ull";
  return stream.str();
}

static uint64_t constant_u64(ConstantInt *constant) {
  return constant->getZExtValue();
}

static string mask_for_width(unsigned width) {
  if (width >= 64) {
    return "0xffffffffffffffffull";
  }
  return hex_u64((1ull << width) - 1ull);
}

static string fit_expr(const string &expr, unsigned width) {
  if (width >= 64) {
    return "(" + expr + ")";
  }
  return "((" + expr + ") & " + mask_for_width(width) + ")";
}

static string sext_expr(const string &expr, unsigned width) {
  if (width >= 64) {
    return "(" + expr + ")";
  }
  uint64_t sign = 1ull << (width - 1);
  string fitted = fit_expr(expr, width);
  return "(((" + fitted + ") ^ " + hex_u64(sign) + ") - " + hex_u64(sign) + ")";
}

static string cxx_value_expr(Value *value, const BatchCandidate &candidate) {
  if (auto *constant = dyn_cast<ConstantInt>(value)) {
    return hex_u64(constant_u64(constant));
  }
  if (isa<ConstantPointerNull>(value)) {
    return "0ull";
  }
  if (auto *instruction = dyn_cast<Instruction>(value)) {
    auto name = candidate.names.find(instruction);
    if (name != candidate.names.end()) {
      return name->second;
    }
  }

  auto input = candidate.input_index.find(value);
  if (input == candidate.input_index.end()) {
    return "0ull";
  }
  static const char *names[] = {"a", "b", "c"};
  return names[input->second];
}

static string cxx_operand(Value *value, const BatchCandidate &candidate, bool sign_extend) {
  string expr = cxx_value_expr(value, candidate);
  unsigned width = value_width(value);
  return sign_extend ? sext_expr(expr, width) : fit_expr(expr, width);
}

static string serialize_value(Value *value, const BatchCandidate &candidate);

static string serialize_instruction(Instruction *instruction, const BatchCandidate &candidate) {
  stringstream stream;
  stream << "(";
  if (auto *binary = dyn_cast<BinaryOperator>(instruction)) {
    stream << operation_for_binary(binary->getOpcode()) << value_width(instruction);
  } else if (auto *icmp = dyn_cast<ICmpInst>(instruction)) {
    stream << operation_for_icmp(icmp->getPredicate()) << value_width(icmp->getOperand(0));
  } else if (isa<SelectInst>(instruction)) {
    stream << "select" << value_width(instruction);
  } else {
    stream << "unsupported";
  }
  for (Use &operand : instruction->operands()) {
    stream << "," << serialize_value(operand.get(), candidate);
  }
  stream << ")";
  return stream.str();
}

static string serialize_value(Value *value, const BatchCandidate &candidate) {
  if (auto *constant = dyn_cast<ConstantInt>(value)) {
    stringstream stream;
    stream << "C" << constant->getBitWidth() << ":" << hex_u64(constant_u64(constant));
    return stream.str();
  }
  if (isa<ConstantPointerNull>(value)) {
    return "CP:null";
  }
  if (auto *instruction = dyn_cast<Instruction>(value)) {
    if (candidate.names.count(instruction)) {
      return serialize_instruction(instruction, candidate);
    }
  }
  auto input = candidate.input_index.find(value);
  if (input == candidate.input_index.end()) {
    return "I?";
  }
  stringstream stream;
  stream << "I" << input->second << ":" << value_width(value);
  return stream.str();
}

static uint64_t fnv1a64(const string &text) {
  uint64_t hash = 1469598103934665603ull;
  for (char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

static uint64_t opcode_for_batch(const string &key, const UopConfig &config,
                                 BatchState &state) {
  auto existing = state.opcode_by_key.find(key);
  if (existing != state.opcode_by_key.end()) {
    return existing->second;
  }

  stringstream seed;
  seed << key << ":" << hex << config.encode_op_xor;
  uint64_t opcode = fnv1a64(seed.str());
  unsigned retry = 0;
  while (opcode == 0 || state.used_opcodes.count(opcode)) {
    opcode = fnv1a64(seed.str() + ":" + to_string(++retry));
  }
  state.used_opcodes.insert(opcode);
  state.opcode_by_key[key] = opcode;
  return opcode;
}

static string cxx_node_expr(Instruction *instruction, const BatchCandidate &candidate) {
  if (auto *binary = dyn_cast<BinaryOperator>(instruction)) {
    string operation = operation_for_binary(binary->getOpcode());
    unsigned width = value_width(binary);
    bool signed_lhs = binary->getOpcode() == Instruction::AShr ||
                      binary->getOpcode() == Instruction::SDiv ||
                      binary->getOpcode() == Instruction::SRem;
    bool signed_rhs = binary->getOpcode() == Instruction::SDiv ||
                      binary->getOpcode() == Instruction::SRem;
    string lhs = cxx_operand(binary->getOperand(0), candidate, signed_lhs);
    string rhs = cxx_operand(binary->getOperand(1), candidate, signed_rhs);

    if (operation == "add") return fit_expr("(" + lhs + " + " + rhs + ")", width);
    if (operation == "sub") return fit_expr("(" + lhs + " - " + rhs + ")", width);
    if (operation == "xor") return fit_expr("(" + lhs + " ^ " + rhs + ")", width);
    if (operation == "and") return fit_expr("(" + lhs + " & " + rhs + ")", width);
    if (operation == "or") return fit_expr("(" + lhs + " | " + rhs + ")", width);
    if (operation == "mul") return fit_expr("(" + lhs + " * " + rhs + ")", width);
    if (operation == "lshr") return fit_expr("(" + lhs + " >> (" + rhs + " & 63ull))", width);
    if (operation == "shl") return fit_expr("(" + lhs + " << (" + rhs + " & 63ull))", width);
    if (operation == "ashr") {
      return fit_expr("static_cast<uint64_t>(static_cast<int64_t>(" + lhs + ") >> (" + rhs + " & 63ull))", width);
    }
    if (operation == "udiv") {
      return fit_expr("(" + lhs + " / ((" + rhs + ") | LOKI_IS_ZERO64(" + rhs + ")))", width);
    }
    if (operation == "urem") {
      return fit_expr("(" + lhs + " % ((" + rhs + ") | LOKI_IS_ZERO64(" + rhs + ")))", width);
    }
    if (operation == "sdiv") {
      return fit_expr("static_cast<uint64_t>(static_cast<int64_t>(" + lhs + ") / static_cast<int64_t>((" + rhs + ") | LOKI_IS_ZERO64(" + rhs + ")))", width);
    }
    if (operation == "srem") {
      return fit_expr("static_cast<uint64_t>(static_cast<int64_t>(" + lhs + ") % static_cast<int64_t>((" + rhs + ") | LOKI_IS_ZERO64(" + rhs + ")))", width);
    }
  }

  if (auto *icmp = dyn_cast<ICmpInst>(instruction)) {
    string operation = operation_for_icmp(icmp->getPredicate());
    bool signed_compare = is_signed_icmp(icmp->getPredicate());
    string lhs = cxx_operand(icmp->getOperand(0), candidate, signed_compare);
    string rhs = cxx_operand(icmp->getOperand(1), candidate, signed_compare);
    if (operation == "eq") return "LOKI_IS_ZERO64((" + lhs + ") ^ (" + rhs + "))";
    if (operation == "ne") return "(LOKI_IS_ZERO64((" + lhs + ") ^ (" + rhs + ")) ^ 1ull)";
    if (operation == "ult") return "LOKI_ULT64(" + lhs + ", " + rhs + ")";
    if (operation == "ule") return "(LOKI_ULT64(" + lhs + ", " + rhs + ") | LOKI_IS_ZERO64((" + lhs + ") ^ (" + rhs + ")))";
    if (operation == "ugt") return "LOKI_ULT64(" + rhs + ", " + lhs + ")";
    if (operation == "uge") return "(LOKI_ULT64(" + rhs + ", " + lhs + ") | LOKI_IS_ZERO64((" + lhs + ") ^ (" + rhs + ")))";
    if (operation == "slt") return "LOKI_SLT64(" + lhs + ", " + rhs + ")";
    if (operation == "sle") return "(LOKI_SLT64(" + lhs + ", " + rhs + ") | LOKI_IS_ZERO64((" + lhs + ") ^ (" + rhs + ")))";
    if (operation == "sgt") return "LOKI_SLT64(" + rhs + ", " + lhs + ")";
    if (operation == "sge") return "(LOKI_SLT64(" + rhs + ", " + lhs + ") | LOKI_IS_ZERO64((" + lhs + ") ^ (" + rhs + ")))";
  }

  if (auto *select = dyn_cast<SelectInst>(instruction)) {
    unsigned width = value_width(select);
    string cond = cxx_operand(select->getCondition(), candidate, false);
    string true_value = cxx_operand(select->getTrueValue(), candidate, false);
    string false_value = cxx_operand(select->getFalseValue(), candidate, false);
    string mask = "(0ull - ((" + cond + ") & 1ull))";
    return fit_expr("((" + true_value + " & " + mask + ") | (" + false_value + " & ~" + mask + "))", width);
  }

  return "0ull";
}

static string emit_batch_cpp(BatchCandidate &candidate, uint64_t opcode,
                             const string &name) {
  stringstream out;
  out << "    {\n";
  for (size_t index = 0; index < candidate.nodes.size(); ++index) {
    Instruction *node = candidate.nodes[index];
    candidate.names[node] = name + "_v" + to_string(index);
    out << "        uint64_t " << candidate.names[node] << " = "
        << cxx_node_expr(node, candidate) << ";\n";
  }
  string result = candidate.names[candidate.root];
  out << "        r |= LOKI_MASK_EQ(op, " << hex_u64(opcode) << ") & "
      << fit_expr(result, value_width(candidate.root)) << ";\n";
  out << "    }\n";
  return out.str();
}

static bool build_batch_candidate(Instruction *root, const set<Instruction *> &consumed,
                                  unsigned max_nodes, BatchCandidate &candidate) {
  if (!root || !root->getParent() || !is_batch_supported_instruction(root)) {
    return false;
  }
  candidate = BatchCandidate();
  candidate.root = root;
  candidate.max_nodes = max_nodes;
  if (!collect_batch_instruction(root, root->getParent(), consumed, candidate, true)) {
    return false;
  }
  if (candidate.nodes.size() < 2 || candidate.inputs.size() > 3) {
    return false;
  }
  candidate.key = serialize_instruction(root, candidate);
  return true;
}

static Value *call_loki_vm(IRBuilder<> &builder, FunctionCallee dispatch,
                           uint64_t opcode, ArrayRef<Value *> inputs,
                           const UopConfig &config, const string &name) {
  Value *args[3] = {builder.getInt64(0), builder.getInt64(0), builder.getInt64(0)};
  for (size_t index = 0; index < inputs.size() && index < 3; ++index) {
    Value *arg = to_i64_unsigned(builder, inputs[index]);
    if (!arg) {
      return nullptr;
    }
    args[index] = arg;
  }

  return builder.CreateCall(
      dispatch,
      {builder.getInt64(opcode ^ config.encode_op_xor),
       encode_i64(builder, args[0], config.encode_a_xor),
       encode_i64(builder, args[1], config.encode_b_xor),
       encode_i64(builder, args[2], config.encode_c_xor)},
      name);
}

static bool replace_batch(BatchCandidate &candidate, FunctionCallee dispatch,
                          const UopConfig &config, BatchState &batch_state) {
  uint64_t opcode = opcode_for_batch(candidate.key, config, batch_state);
  string name;
  auto name_it = batch_state.name_by_key.find(candidate.key);
  if (name_it == batch_state.name_by_key.end()) {
    name = "batch_" + to_string(batch_state.emitted++);
    batch_state.name_by_key[candidate.key] = name;
    batch_state.cpp += emit_batch_cpp(candidate, opcode, name);
  } else {
    name = name_it->second;
  }

  IRBuilder<> builder(candidate.root);
  Value *call = call_loki_vm(builder, dispatch, opcode, candidate.inputs, config, "loki.batch");
  if (!call) {
    return false;
  }
  Value *replacement = from_i64(builder, call, candidate.root->getType());
  if (!replacement) {
    return false;
  }

  candidate.root->replaceAllUsesWith(replacement);
  for (auto it = candidate.nodes.rbegin(); it != candidate.nodes.rend(); ++it) {
    if ((*it)->getParent()) {
      (*it)->eraseFromParent();
    }
  }
  return true;
}

static FunctionStats virtualize_function(Function &function, FunctionCallee dispatch,
                                         const UopConfig &config,
                                         BatchState *batch_state,
                                         const Options &options) {
  vector<Instruction *> instructions;
  for (auto &instruction : llvm::instructions(function)) {
    instructions.push_back(&instruction);
  }

  FunctionStats stats;
  set<Instruction *> consumed;
  bool use_batching = batch_state && options.enable_batching && !options.batch_cpp_path.empty();

  for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
    Instruction *instruction = *it;
    if (consumed.count(instruction)) {
      continue;
    }
    if (!instruction->getParent()) {
      continue;
    }

    if (use_batching) {
      BatchCandidate candidate;
      if (build_batch_candidate(instruction, consumed, options.max_batch_nodes, candidate) &&
          replace_batch(candidate, dispatch, config, *batch_state)) {
        for (Instruction *node : candidate.nodes) {
          consumed.insert(node);
        }
        stats.virtualized_uops += candidate.nodes.size();
        stats.vm_entries += 1;
        stats.batches += 1;
        continue;
      }
    }

    bool replaced = false;
    if (auto *binary = dyn_cast<BinaryOperator>(instruction)) {
      replaced = replace_binary(binary, dispatch, config);
    } else if (auto *icmp = dyn_cast<ICmpInst>(instruction)) {
      replaced = replace_icmp(icmp, dispatch, config);
    } else if (auto *select = dyn_cast<SelectInst>(instruction)) {
      replaced = replace_select(select, dispatch, config);
    }
    if (replaced) {
      consumed.insert(instruction);
      stats.virtualized_uops += 1;
      stats.vm_entries += 1;
    }
  }

  if (stats.virtualized_uops > 0) {
    function.removeFnAttr(Attribute::ReadNone);
    function.removeFnAttr(Attribute::ReadOnly);
    function.removeFnAttr(Attribute::ArgMemOnly);
    function.removeFnAttr(Attribute::InaccessibleMemOnly);
    function.removeFnAttr(Attribute::InaccessibleMemOrArgMemOnly);
  }

  return stats;
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
  BatchState batch_state(options.uop_config);
  BatchState *batch_state_ptr = options.batch_cpp_path.empty() ? nullptr : &batch_state;
  size_t total_replaced = 0;
  size_t total_entries = 0;
  size_t total_batches = 0;
  for (Function *target : targets) {
    FunctionStats stats = virtualize_function(*target, dispatch, options.uop_config,
                                              batch_state_ptr, options);
    total_replaced += stats.virtualized_uops;
    total_entries += stats.vm_entries;
    total_batches += stats.batches;
    errs() << "virtualized " << stats.virtualized_uops << " uops in "
           << target->getName();
    if (stats.batches > 0) {
      errs() << " (" << stats.vm_entries << " VM entries, " << stats.batches
             << " batched)";
    }
    errs() << "\n";
  }

  if (!options.batch_cpp_path.empty()) {
    error_code batch_ec;
    raw_fd_ostream batch_output(options.batch_cpp_path, batch_ec, sys::fs::F_None);
    if (batch_ec) {
      errs() << "Could not open batch output: " << batch_ec.message() << "\n";
      return 1;
    }
    batch_output << batch_state.cpp;
    errs() << "formed " << total_batches << " batched VM entries; " << total_entries
           << " total VM entries\n";
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
