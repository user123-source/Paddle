/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/operators/sequence_ops/sequence_softmax_op.h"

#include <string>

#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
#include "paddle/phi/backends/gpu/gpu_dnn.h"
#endif

namespace paddle {
namespace operators {

class SequenceSoftmaxOp : public framework::OperatorWithKernel {
 public:
  using framework::OperatorWithKernel::OperatorWithKernel;

  void InferShape(framework::InferShapeContext* ctx) const override {
    OP_INOUT_CHECK(ctx->HasInput("X"), "Input", "X", "SequenceSoftmax");
    OP_INOUT_CHECK(ctx->HasOutput("Out"), "Output", "Out", "SequenceSoftmax");

    ctx->ShareDim("X", /*->*/ "Out");
    ctx->ShareLoD("X", /*->*/ "Out");
  }

 protected:
  phi::KernelKey GetExpectedKernelType(
      const framework::ExecutionContext& ctx) const override {
    auto input_data_type = OperatorWithKernel::IndicateVarDataType(ctx, "X");
    phi::DataLayout layout_ = DataLayout::kAnyLayout;
    if (ctx.HasAttr("data_format")) {
      layout_ =
          common::StringToDataLayout(ctx.Attr<std::string>("data_format"));
    }
    return phi::KernelKey(
        ctx.GetPlace(), layout_, phi::TransToPhiDataType(input_data_type));
  }
};

class SequenceSoftmaxOpMaker : public framework::OpProtoAndCheckerMaker {
 public:
  void Make() override {
    AddInput("X",
             "(LoDTensor) 1-D or 2-D input LoDTensor with the 2-nd dimension "
             "of length 1.");
    AddOutput("Out",
              "(LoDTensor) 1-D or 2-D output LoDTensor with the 2-nd dimension "
              "of length 1.");
    AddAttr<bool>(
        "use_cudnn",
        "(bool, default false) Only used in cudnn kernel, need install cudnn")
        .SetDefault(false)
        .AsExtra();
    AddComment(R"DOC(
Sequence Softmax Operator.

SequenceSoftmaxOp computes the softmax activation among all time-steps for each
sequence. The dimension of each time-step should be 1. Thus, the shape of
input Tensor can be either [N, 1] or [N], where N is the sum of the length
of all sequences.

The algorithm works as follows:

    for i-th sequence in a mini-batch:

$$
Out(X[lod[i]:lod[i+1]], :) = \
\frac{\exp(X[lod[i]:lod[i+1], :])} \
{\sum(\exp(X[lod[i]:lod[i+1], :]))}
$$

For example, for a mini-batch of 3 sequences with variable-length,
each containing 2, 3, 2 time-steps, the lod of which is [0, 2, 5, 7],
then softmax will be computed among X[0:2, :], X[2:5, :], X[5:7, :]
and N turns out to be 7.

)DOC");
  }
};

class SequenceSoftmaxGradOp : public framework::OperatorWithKernel {
 public:
  using framework::OperatorWithKernel::OperatorWithKernel;

  void InferShape(framework::InferShapeContext* ctx) const override {
    OP_INOUT_CHECK(ctx->HasInput("Out"), "Input", "Out", "SequenceSoftmaxGrad");
    OP_INOUT_CHECK(ctx->HasInput(framework::GradVarName("Out")),
                   "Input",
                   "Out@GRAD",
                   "SequenceSoftmaxGrad");
    OP_INOUT_CHECK(ctx->HasInput("X"), "Input", "X", "SequenceSoftmaxGrad");
    OP_INOUT_CHECK(ctx->HasOutput(framework::GradVarName("X")),
                   "Output",
                   "X@GRAD",
                   "SequenceSoftmaxGrad");

    auto out_dim = ctx->GetInputDim("Out");
    auto out_grad_dim = ctx->GetInputDim(framework::GradVarName("Out"));
    PADDLE_ENFORCE_EQ(
        out_dim,
        out_grad_dim,
        phi::errors::InvalidArgument(
            "The shape of Input(Out) and Input(Out@GRAD) of "
            "SequenceSoftmaxGrad operator do not match. The Input(Out)'s shape "
            "is [%s], the Input(Out@GRAD)'s shape is [%s].",
            out_dim,
            out_grad_dim));

    ctx->SetOutputDim(framework::GradVarName("X"), ctx->GetInputDim("X"));
  }

 protected:
  phi::KernelKey GetExpectedKernelType(
      const framework::ExecutionContext& ctx) const override {
    auto input_data_type = OperatorWithKernel::IndicateVarDataType(ctx, "Out");
    phi::DataLayout layout_ = DataLayout::kAnyLayout;
    if (ctx.HasAttr("data_format")) {
      layout_ =
          common::StringToDataLayout(ctx.Attr<std::string>("data_format"));
    }
    return phi::KernelKey(
        ctx.GetPlace(), layout_, phi::TransToPhiDataType(input_data_type));
  }
};

DECLARE_NO_NEED_BUFFER_VARS_INFERER(
    SequenceSoftmaxGradOpNoNeedBufferVarsInferer, "X");

}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;
REGISTER_OPERATOR(
    sequence_softmax,
    ops::SequenceSoftmaxOp,
    ops::SequenceSoftmaxOpMaker,
    paddle::framework::DefaultGradOpMaker<paddle::framework::OpDesc, true>,
    paddle::framework::DefaultGradOpMaker<paddle::imperative::OpBase, true>);
REGISTER_OPERATOR(sequence_softmax_grad,
                  ops::SequenceSoftmaxGradOp,
                  ops::SequenceSoftmaxGradOpNoNeedBufferVarsInferer);
PD_REGISTER_STRUCT_KERNEL(sequence_softmax,
                          CPU,
                          ALL_LAYOUT,
                          ops::SequenceSoftmaxKernel,
                          float,
                          double) {}
PD_REGISTER_STRUCT_KERNEL(sequence_softmax_grad,
                          CPU,
                          ALL_LAYOUT,
                          ops::SequenceSoftmaxGradKernel,
                          float,
                          double) {}
