#include <torch/csrc/jit/passes/onnx/scalar_type_analysis.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>

namespace torch {
namespace jit {

namespace onnx {
using namespace ::c10::onnx;
}

namespace {
class ScalarTypeHashFunction {
 public:
  size_t operator()(const c10::ScalarType& type) const {
    return static_cast<size_t>(type);
  }
};

static const std::unordered_map<c10::ScalarType, int, ScalarTypeHashFunction>
    scalarTypeToONNXTypeMap = {
        {c10::kFloat, 1},
        {c10::kByte, 2},
        {c10::kChar, 3},
        {c10::kShort, 5},
        {c10::kInt, 6},
        {c10::kLong, 7},
        {c10::kBool, 9},
        {c10::kHalf, 10},
        {c10::kDouble, 11},
};

static int64_t ScalarTypeToONNXType(const c10::ScalarType& st) {
  int64_t onnx_type = -1;
  const auto it = scalarTypeToONNXTypeMap.find(st);
  if (it != scalarTypeToONNXTypeMap.end()) {
    onnx_type = it->second;
  }
  return onnx_type;
}

// For these operators, all inputs and outputs share the same scalar type.
// There is no operator-wise special case handling needed.
static const std::unordered_set<NodeKind> standardOps = {
    onnx::Add,
    onnx::Sub,
    onnx::Mul,
    onnx::Div,
    onnx::Gemm,
    onnx::Pow,
    onnx::Mod,
};

static bool IsStandardOp(const NodeKind& nkind) {
  return standardOps.find(nkind) != standardOps.end();
}

// For these operators, all inputs share the same scalar type.
// The output scalar type is always Bool.
static const std::unordered_set<NodeKind> comparisonOps = {onnx::Greater,
                                                           onnx::Less,
                                                           onnx::Equal,
                                                           onnx::GreaterOrEqual,
                                                           onnx::LessOrEqual};

static bool IsComparisonOp(const NodeKind& nkind) {
  return comparisonOps.find(nkind) != comparisonOps.end();
}

static TensorTypePtr CreateProfiledTensorTypeWithScalarType(
    const TensorTypePtr& typePtr,
    const c10::ScalarType& scalar_type) {
  return typePtr->withScalarType({scalar_type});
}

static bool IsImplicitCastSupported(const NodeKind& nodeKind) {
  return (
      standardOps.find(nodeKind) != standardOps.end() ||
      comparisonOps.find(nodeKind) != comparisonOps.end());
}

static c10::optional<c10::ScalarType> PromoteScalarTypes(
    const std::vector<c10::ScalarType>& types) {
  if (types.empty()) {
    return c10::nullopt;
  }
  auto st = types[0];
  for (size_t i = 1; i < types.size(); ++i) {
    st = c10::promoteTypes(st, types[i]);
  }
  return st;
}

static c10::optional<c10::ScalarType> InferExpectedScalarType(const Node* n) {
  std::vector<c10::ScalarType> typesFromTensors;
  std::vector<c10::ScalarType> typesFromScalars;
  std::for_each(
      n->inputs().begin(), n->inputs().end(), [&](const Value* input) {
        auto nkind = input->node()->kind();
        if (nkind == onnx::Gather &&
            input->node()->input(0)->node()->kind() == onnx::Shape) {
          // This is a special pattern generated by code like `dim_size =
          // x.size(0)`. It gets converted to the below ONNX IR graph
          //    %1 : Long() = onnx::Constant[value={0}]()
          //    %2 : Tensor = onnx::Shape(%x)
          //    %dim_size : Long() = onnx::Gather(%2, %1)
          // `dim_size` is treated in PyTorch as Scalar.
          // However, in the ONNX IR graph, it is an output of onnx::Gather,
          // which is by default considered as a tensor.
          typesFromScalars.emplace_back(c10::kLong);
        } else if (nkind == onnx::Constant) {
          typesFromScalars.emplace_back(
              input->node()->t(attr::value).scalar_type());
        } else if (
            auto scalar_type =
                input->type()->cast<TensorType>()->scalarType()) {
          typesFromTensors.emplace_back(*scalar_type);
        }
      });

  c10::optional<c10::ScalarType> st = c10::nullopt;
  const c10::optional<c10::ScalarType> output_st =
      n->output()->type()->cast<TensorType>()->scalarType();

  if (IsComparisonOp(n->kind())) {
    // For comparison ops, always promote scalar type to highest among inputs,
    // regardless if that input is a tensor or scalar.
    typesFromScalars.insert(
        typesFromScalars.end(),
        typesFromTensors.begin(),
        typesFromTensors.end());
    st = PromoteScalarTypes(typesFromScalars);
  } else {
    if (typesFromScalars.size() == n->inputs().size()) {
      // If all inputs are scalars, infer scalar_type by calling
      // c10::promoteTypes.
      st = PromoteScalarTypes(typesFromScalars);
    } else if (output_st) {
      // If output scalar type is available, use that.
      st = output_st;
    } else if (!typesFromTensors.empty()) {
      // When inputs consist of tensors and scalars. In PyTorch, scalars are
      // implicitly casted to have the same scalar type as input tensors.
      st = typesFromTensors[0];
      if (std::any_of(
              typesFromTensors.begin(),
              typesFromTensors.end(),
              [&st](const c10::ScalarType& type) { return type != st; })) {
        std::cerr
            << "Warning: ONNX Scalar Type Analysis - Scalar types mismatch for tensor inputs of operator "
            << n->kind().toDisplayString()
            << ". Please report a bug to PyTorch. "
            << "The scalar type " << c10::toString(*st)
            << " of the first tensor is chosen." << std::endl;
      }
    } else {
      // When inputs consist of only scalars.
      st = PromoteScalarTypes(typesFromScalars);
    }
  }

  return st;
}

static void UpdateScalarTypeForInputs(
    Node* n,
    const c10::ScalarType& scalar_type) {
  const int64_t onnx_type = ScalarTypeToONNXType(scalar_type);
  if (onnx_type < 0) {
    std::cerr << "Warning: ONNX Scalar Type Analysis - Scalar type: "
              << c10::toString(scalar_type)
              << " of input tensor in operator: " << n->kind().toDisplayString()
              << " not supported in ONNX. " << std::endl;
    return;
  }

  for (auto input : n->inputs()) {
    auto input_tensor_type = input->type()->cast<TensorType>();
    auto input_scalar_type = input_tensor_type->scalarType();

    if ((input->node()->kind() == onnx::Constant) ||
        (input_scalar_type && (*input_scalar_type != scalar_type))) {
      if (input->node()->kind() == onnx::Constant) {
        // Fix up the scalar directly instead of inserting a cast operator.
        // NOTE: Keep only the else branch once constant_folding is enabled by
        // default.
        at::Tensor val = input->node()->t(attr::value);
        at::Tensor new_val = val.to(scalar_type);
        Node* const_node = n->owningGraph()->create(onnx::Constant);
        const_node->t_(attr::value, new_val);
        const_node->insertBefore(n);
        const_node->output()->setType(TensorType::create(new_val));
        n->replaceInputWith(input, const_node->output());
      } else {
        Node* cast_node = n->owningGraph()->create(onnx::Cast);
        cast_node->addInput(input);
        cast_node->i_(attr::to, onnx_type);
        cast_node->insertBefore(n);
        cast_node->output()->setType(CreateProfiledTensorTypeWithScalarType(
            input_tensor_type, scalar_type));
        n->replaceInputWith(input, cast_node->output());
      }
    }
  }
}

static void UpdateScalarTypeForOutput(
    Node* n,
    const c10::ScalarType& scalar_type) {
  auto output_tensor_type = n->output()->type()->cast<TensorType>();
  n->output()->setType(
      CreateProfiledTensorTypeWithScalarType(output_tensor_type, scalar_type));
}

static void ImplicitCastForONNX(Block* block) {
  for (auto it = block->nodes().begin(); it != block->nodes().end(); ++it) {
    for (auto sub : it->blocks()) {
      ImplicitCastForONNX(sub);
    }
    auto* subgraph = it->owningGraph();

    if (IsImplicitCastSupported(it->kind())) {
      auto expected_scalar_type = InferExpectedScalarType(*it);
      if (expected_scalar_type) {
        UpdateScalarTypeForInputs(*it, *expected_scalar_type);
        if (!IsComparisonOp(it->kind())) {
          UpdateScalarTypeForOutput(*it, *expected_scalar_type);
        }
      }
    }
  }
  EliminateDeadCode(
      block, true, DCESideEffectPolicy::ALLOW_DELETING_NODES_WITH_SIDE_EFFECTS);
}

// This pass tries to resolve scalar type mismatch issues between input tensors
// introduced by the implicit type conversions on scalars.
// TODO: Note that currently this pass handles traced graph only.
// More specifically, graphs that have scalar type information recorded.
// For scripted graphs we need something like scalar type propagation,
// otherwise we do not have enough information to perform the check, let alone
// fixes.
void ImplicitCastForONNX(const std::shared_ptr<Graph>& graph) {
  ImplicitCastForONNX(graph->block());
}
} // anonymous namespace

void ScalarTypeAnalysisForONNX(const std::shared_ptr<Graph>& graph) {
  ImplicitCastForONNX(graph->block());
}

} // namespace jit
} // namespace torch
