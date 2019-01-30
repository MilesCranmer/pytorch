#pragma once

#include <vector>
#include <ATen/core/function_schema.h>

namespace caffe2 {
namespace detail {

template<class Caffe2Operator> const c10::OperatorHandle& c10_op_handle_for_c2_op();
template <class Caffe2Operator>
void call_caffe2_op_from_c10(c10::Stack* stack, c10::KernelCache* cache) { // TODO Pass in correct cache type
  // precondition: on the stack, there's an IValue for each input and an IValue for each output.
  // The output ones could either be a preallocated tensor or ivalue::None.

  const auto& schema = c10_op_handle_for_c2_op<Caffe2Operator>().schema();
  const size_t num_outputs = schema.returns().size();
  const size_t total_num_arguments = schema.arguments().size();
  const size_t num_inputs = total_num_arguments - num_outputs;

  c10::ArrayRef<c10::IValue> inputs = torch::jit::peekSlice(*stack, 0, num_inputs, total_num_arguments);
  c10::ArrayRef<c10::IValue> outputs = torch::jit::peekSlice(*stack, num_inputs, num_outputs, total_num_arguments);

  auto inputsVec = inputs.vec();
  auto outputsVec_ = outputs.vec();

  const auto device = at::Device(at::DeviceType::CPU); // TODO Handle GPU devices

  for (auto& output : outputsVec_) {
    if (output.isNone() || (output.isTensor() && !output.toTensor().defined())) {
      output = at::Tensor(c10::C10Tensor(caffe2::empty({0}, device)));
    }
  }

  std::vector<c10::IValue*> outputsVec;
  outputsVec.reserve(outputsVec_.size());
  for (auto& output : outputsVec_) {
    outputsVec.push_back(&output);
  }

  Caffe2Operator(caffe2::LayerNorm().schema(), std::move(inputsVec), std::move(outputsVec)).Run();

  torch::jit::drop(*stack, total_num_arguments);
  for (auto& output: outputsVec_) {
    torch::jit::push(*stack, std::move(output));
  }

  // postcondition: All inputs are cleared from the stack, there's now one
  //                IValue for each output which holds the result. This
  //                might reuse one of the preallocated tensors but doesn't have to.
}

c10::FunctionSchema make_function_schema_for_c10(const char* OperatorName, std::vector<c10::Argument> inputs, std::vector<c10::Argument> outputs) {
  // actual_inputs is the real inputs plus an optional tensor argument for each output.
  // this can be used to pass in a preallocated output tensor.
  std::vector<c10::Argument> actual_inputs = inputs;
  actual_inputs.reserve(inputs.size() + outputs.size());
  for (const auto& elem : outputs) {
    AT_ASSERT(elem.type()->kind() == TypeKind::DynamicType); // DynamicType means type is tensor
    actual_inputs.push_back(c10::Argument(elem.name(), OptionalType::ofTensor(), nullopt, IValue()));
  }

  return c10::FunctionSchema(
    std::string("_caffe2::") + OperatorName,
    std::move(actual_inputs), std::move(outputs));
}

}
}

/**
 * Call this macro to register a caffe2 operator with the c10 dispatcher.
 */
// TODO This macro should take a JIT schema string instead of a vector of inputs and outputs.
#define C10_REGISTER_CAFFE2_OPERATOR(OperatorName, Inputs, Outputs, OperatorClass)                \
  /* Register the op schema with the c10 dispatcher */                                            \
  namespace caffe2 {                                                                              \
    C10_DEFINE_OP_SCHEMA(OperatorName,                                                            \
      caffe2::detail::make_function_schema_for_c10(                                               \
        #OperatorName, Inputs, Outputs));                                                         \
  }                                                                                               \
  /* Store the c10 operator handle so call_caffe2_op_from_c10 can access it */                    \
  namespace caffe2 { namespace detail {                                                           \
  template<>                                                                                      \
  const c10::OperatorHandle& c10_op_handle_for_c2_op<caffe2::LayerNormOp<caffe2::CPUContext>>() { \
    return caffe2::OperatorName();                                                                \
  }                                                                                               \
  }}                                                                                              \
  /* Register call_caffe2_op_from_c10 as a kernel with the c10 dispatcher */                      \
  namespace c10 {                                                                                 \
  C10_REGISTER_KERNEL(caffe2::OperatorName)                                                       \
      /*.withCache<Cache>()*/                                                                     \
      .kernel<&caffe2::detail::call_caffe2_op_from_c10<OperatorClass<caffe2::CPUContext>>>()      \
      .dispatchKey(CPUTensorId());                                                                \
  }