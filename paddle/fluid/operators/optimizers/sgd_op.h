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

#pragma once

#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/framework/selected_rows_utils.h"
#include "paddle/fluid/framework/var_type_traits.h"
#include "paddle/phi/common/bfloat16.h"
#include "paddle/phi/kernels/funcs/eigen/common.h"
#include "paddle/phi/kernels/funcs/jit/kernels.h"

namespace paddle {
namespace operators {

namespace detail {

template <typename T, int VariableTypeId>
struct sgd_dense_param_kernel {
  void operator()() const {}
};

// LodTensor
template <typename T>
struct sgd_dense_param_kernel<T,
                              framework::VarTypeTrait<phi::DenseTensor>::kId> {
  void operator()(const framework::ExecutionContext &ctx) const {
    VLOG(4) << "[CPU]: sgd_dense_param_kernel<T, phi::DenseTensor>";
    const auto *learning_rate = ctx.Input<phi::DenseTensor>("LearningRate");
    const auto *param = ctx.Input<phi::DenseTensor>("Param");
    auto *param_out = ctx.Output<phi::DenseTensor>("ParamOut");
    const auto *grad = ctx.Input<phi::DenseTensor>("Grad");

    const auto sz = param_out->numel();
    phi::jit::sgd_attr_t attr(1, sz, 1, sz, 1);
    const T *lr = learning_rate->data<T>();
    const T *param_data = param->data<T>();
    const T *grad_data = grad->data<T>();
    int64_t rows_idx = 0;
    T *out_data = param_out->mutable_data<T>(ctx.GetPlace());

    auto sgd =
        phi::jit::KernelFuncs<phi::jit::SgdTuple<T>, phi::CPUPlace>::Cache().At(
            attr);
    sgd(lr, param_data, grad_data, &rows_idx, out_data, &attr);
  }
};

// SelectedRows
template <typename T>
struct sgd_dense_param_kernel<T,
                              framework::VarTypeTrait<phi::SelectedRows>::kId> {
  void operator()(const framework::ExecutionContext &ctx) const {
    VLOG(4) << "[CPU]: sgd_dense_param_kernel<T, SelectedRows>";
    const auto *learning_rate = ctx.Input<phi::DenseTensor>("LearningRate");
    const auto *param = ctx.Input<phi::DenseTensor>("Param");
    auto *param_out = ctx.Output<phi::DenseTensor>("ParamOut");
    const auto *grad = ctx.Input<phi::SelectedRows>("Grad");

    const auto &grad_value = grad->value();
    const auto &grad_rows = grad->rows();
    const T *param_data = param->data<T>();
    const T *grad_data = grad_value.data<T>();
    const T *lr = learning_rate->data<T>();
    const int64_t *rows_data = grad_rows.data();
    T *out_data = param_out->mutable_data<T>(ctx.GetPlace());

    phi::jit::sgd_attr_t attr;
    attr.param_height = param_out->dims()[0];
    attr.param_width = param_out->numel() / attr.param_height;
    attr.grad_height = grad_rows.size();  // note: it is not grad->height()
    attr.grad_width = grad_value.numel() / attr.grad_height;
    attr.selected_rows_size = grad_rows.size();

    auto sgd =
        phi::jit::KernelFuncs<phi::jit::SgdTuple<T>, phi::CPUPlace>::Cache().At(
            attr);
    sgd(lr, param_data, grad_data, rows_data, out_data, &attr);
  }
};

// LodTensor
template <>
struct sgd_dense_param_kernel<phi::dtype::bfloat16,
                              framework::VarTypeTrait<phi::DenseTensor>::kId> {
  void operator()(const framework::ExecutionContext &ctx) const {
    VLOG(4) << "[CPU]: sgd_dense_param_kernel<bfloat16, phi::DenseTensor>";
    const auto *learning_rate = ctx.Input<phi::DenseTensor>("LearningRate");
    const auto *param = ctx.Input<phi::DenseTensor>("Param");
    auto *param_out = ctx.Output<phi::DenseTensor>("ParamOut");
    const auto *grad = ctx.Input<phi::DenseTensor>("Grad");
    param_out->mutable_data<phi::dtype::bfloat16>(ctx.GetPlace());

    auto p = phi::EigenVector<phi::dtype::bfloat16>::Flatten(*param);
    auto g = phi::EigenVector<phi::dtype::bfloat16>::Flatten(*grad);
    auto o = phi::EigenVector<phi::dtype::bfloat16>::Flatten(*param_out);
    const auto *lr = learning_rate->data<phi::dtype::bfloat16>();

    o = p - lr[0] * g;
  }
};

// SelectedRows
template <>
struct sgd_dense_param_kernel<phi::dtype::bfloat16,
                              framework::VarTypeTrait<phi::SelectedRows>::kId> {
  void operator()(const framework::ExecutionContext &ctx) const {
    VLOG(4) << "[CPU]: sgd_dense_param_kernel<bfloat16, SelectedRows>";
    const auto *learning_rate = ctx.Input<phi::DenseTensor>("LearningRate");
    auto *param_out = ctx.Output<phi::DenseTensor>("ParamOut");
    const auto *grad = ctx.Input<phi::SelectedRows>("Grad");

    const auto &grad_value = grad->value();
    const auto &grad_rows = grad->rows();
    const auto grad_height = grad->height();
    const int64_t grad_val_height = static_cast<int64_t>(grad_rows.size());
    const auto grad_width = grad_value.numel() / grad_val_height;

    const auto *grad_data = grad_value.data<phi::dtype::bfloat16>();
    auto *out_data = param_out->data<phi::dtype::bfloat16>();
    const auto *lr = learning_rate->data<phi::dtype::bfloat16>();

    for (size_t i = 0; i < grad_rows.size(); ++i) {
      PADDLE_ENFORCE_LT(
          grad_rows[i],
          grad_height,
          phi::errors::OutOfRange(
              "Grad rows index value should be less than grad height."
              "Got [%s], but expected less than [%s]",
              grad_rows[i],
              grad_height));
      const int64_t row = grad_rows[i];
      for (int64_t j = 0; j < grad_width; ++j) {
        out_data[row * grad_width + j] -= lr[0] * grad_data[i * grad_width + j];
      }
    }
  }
};

}  // namespace detail

template <typename DeviceContext, typename T>
class SGDOpKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &ctx) const override;
};

template <typename T>
class SGDOpKernel<phi::CPUContext, T> : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &ctx) const override {
    const auto *param_var = ctx.InputVar("Param");

    if (param_var->IsType<phi::DenseTensor>()) {
      invoke_dense_param_kernel(ctx);
    } else if (param_var->IsType<phi::SelectedRows>()) {
      sparse_param_and_grad_kernel(ctx);
    } else {
      PADDLE_ENFORCE_EQ(
          false,
          true,
          phi::errors::PermissionDenied(
              "Unsupported Variable Type of Parameter in SgdOp. Excepted "
              "LodTensor or SelectedRows, But received [%s]",
              paddle::framework::ToTypeName(param_var->Type())));
    }
  }

 protected:
  void invoke_dense_param_kernel(const framework::ExecutionContext &ctx) const {
    const auto *param = ctx.Input<phi::DenseTensor>("Param");
    auto *param_out = ctx.Output<phi::DenseTensor>("ParamOut");
    const auto *grad_var = ctx.InputVar("Grad");

    if (grad_var->IsType<phi::DenseTensor>()) {
      const auto *grad = ctx.Input<phi::DenseTensor>("Grad");
      const auto sz = param_out->numel();
      PADDLE_ENFORCE_EQ(param->numel(),
                        sz,
                        phi::errors::InvalidArgument(
                            "The input tensor Param's numel of SgdOp "
                            "should be equal with ParamOut's numel. "
                            "But received Param's "
                            "numel = [%s], ParamOut's numel = [%s]",
                            param->numel(),
                            sz));
      PADDLE_ENFORCE_EQ(
          grad->numel(),
          sz,
          phi::errors::InvalidArgument("The input tensor Grad's numel of SgdOp "
                                       "should be equal with ParamOut's numel. "
                                       "But received Grad's "
                                       "numel = [%s], ParamOut's numel = [%s]",
                                       grad->numel(),
                                       sz));

      dense_param_and_grad_kernel(ctx);
    } else if (grad_var->IsType<phi::SelectedRows>()) {
      // TODO(qijun): In Sparse SGD operator, in-place update is enforced.
      // This manual optimization brings difficulty to track data dependency.
      // It's better to find a more elegant solution.
      PADDLE_ENFORCE_EQ(param,
                        param_out,
                        phi::errors::InvalidArgument(
                            "The input tensor Param of SgdOp "
                            "should be equal with ParamOut if variable's "
                            "type is SelectedRows. "));
      const auto *grad = ctx.Input<phi::SelectedRows>("Grad");

      // for distributed training, a sparse var may be empty,
      // just skip updating.
      if (grad->rows().size() == 0) {
        return;
      }

      auto out_dims = param_out->dims();
      PADDLE_ENFORCE_EQ(
          grad->height(),
          out_dims[0],
          phi::errors::InvalidArgument(
              "The input tensor Grad's height of SgdOp "
              "should be equal with ParamOut's dims. But received  Grad's "
              "height [%s] and ParamOut's dims [%s]",
              grad->height(),
              out_dims[0]));

      auto &grad_value = grad->value();
      auto &grad_rows = grad->rows();
      const auto param_height = param_out->dims()[0];
      const auto param_width = param_out->numel() / param_height;
      // note: it is not grad->height()
      const auto grad_height = static_cast<int64_t>(grad_rows.size());
      const auto grad_width = grad_value.numel() / grad_height;

      PADDLE_ENFORCE_EQ(
          grad_width,
          param_width,
          phi::errors::InvalidArgument(
              "The grad_value's numel of SgdOp "
              "should be equal with param_out's numel. But received "
              "grad_value's numel [%s] and param_out's numel [%s]",
              grad_width,
              param_width));

      dense_param_sparse_grad_kernel(ctx);
    } else {
      PADDLE_ENFORCE_EQ(
          false,
          true,
          phi::errors::PermissionDenied(
              "Unsupported Variable Type of Grad in SgdOp. Excepted "
              "LodTensor or SelectedRows, But received [%s]",
              paddle::framework::ToTypeName(grad_var->Type())));
    }
  }

  void sparse_param_and_grad_kernel(
      const framework::ExecutionContext &ctx) const {
    const auto *learning_rate = ctx.Input<phi::DenseTensor>("LearningRate");
    const auto *param_var = ctx.InputVar("Param");
    const auto *grad_var = ctx.InputVar("Grad");

    PADDLE_ENFORCE_EQ(grad_var->IsType<phi::SelectedRows>(),
                      true,
                      phi::errors::InvalidArgument(
                          "When param is SelectedRows, gradient should also "
                          "be SelectedRows"));
    const auto &param = param_var->Get<phi::SelectedRows>();
    auto *param_out = ctx.Output<phi::SelectedRows>("ParamOut");
    const auto &grad = grad_var->Get<phi::SelectedRows>();

    // for distributed training, a sparse var may be empty,
    // just skip updating.
    if (grad.rows().size() == 0) {
      return;
    }

    auto param_row_width = param.value().dims()[1];
    auto grad_row_width = grad.value().dims()[1];
    PADDLE_ENFORCE_EQ(
        param_row_width,
        grad_row_width,
        phi::errors::InvalidArgument(
            "The param_row in SgdOP should have the same size with grad_row. "
            "But received param_row's width is [%s], and grad_row's width is "
            "[%s]",
            param_row_width,
            grad_row_width));

    const auto *lr = learning_rate->data<T>();
    const auto *grad_data = grad.value().data<T>();
    auto *out_data = param_out->mutable_value()->data<T>();
    for (size_t i = 0; i < grad.rows().size(); i++) {
      int64_t id_index = param_out->AutoGrownIndex(grad.rows()[i], false);
      PADDLE_ENFORCE_GE(
          id_index,
          static_cast<int64_t>(0),
          phi::errors::InvalidArgument(
              "The id in SgdOp should be >= 0. But received id_index is [%s]",
              id_index));
      for (int64_t j = 0; j < grad_row_width; j++) {
        out_data[id_index * grad_row_width + j] -=
            lr[0] * grad_data[i * grad_row_width + j];
      }
    }
  }

  virtual void dense_param_and_grad_kernel(
      const framework::ExecutionContext &ctx) const {
    detail::sgd_dense_param_kernel<
        T,
        framework::VarTypeTrait<phi::DenseTensor>::kId>()(ctx);
  }

  virtual void dense_param_sparse_grad_kernel(
      const framework::ExecutionContext &ctx) const {
    detail::sgd_dense_param_kernel<
        T,
        framework::VarTypeTrait<phi::SelectedRows>::kId>()(ctx);
  }
};

}  // namespace operators
}  // namespace paddle
