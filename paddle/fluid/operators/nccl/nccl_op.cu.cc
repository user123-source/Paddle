/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
http://www.apache.org/licenseshashernless required by applicable law or agreed
to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <functional>
#include <unordered_map>

#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/operators/nccl/nccl_gpu_common.h"

namespace paddle {
namespace operators {

using platform::Communicator;

template <typename Type>
class NCCLTypeWrapper;

template <>
class NCCLTypeWrapper<float> {
 public:
  static const ncclDataType_t type = ncclFloat;
};

template <>
class NCCLTypeWrapper<double> {
 public:
  static const ncclDataType_t type = ncclDouble;
};

static ncclRedOp_t str_to_nccl_red_type(std::string reduction) {
  static const std::unordered_map<std::string, ncclRedOp_t> str_to_type = {
      {"ncclSum", ncclSum},
      {"ncclMin", ncclMin},
      {"ncclMax", ncclMax},
      {"ncclProd", ncclProd},
  };
  auto it = str_to_type.find(reduction);
  PADDLE_ENFORCE_EQ(it != str_to_type.end(),
                    true,
                    phi::errors::InvalidArgument(
                        "Invalid nccl reduction. Must be ncclMin | ncclMax | "
                        "ncclProd | ncclSum"));
  return it->second;
}

template <typename T, typename DeviceContext>
class NCCLAllReduceKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    PADDLE_ENFORCE_EQ(ctx.GetPlace().GetType() == phi::AllocationType::GPU,
                      true,
                      phi::errors::PreconditionNotMet(
                          "This kernel only runs on GPU device."));
    auto* x = ctx.Input<phi::DenseTensor>("X");
    auto* out = ctx.Output<phi::DenseTensor>("Out");
    auto* comm = ctx.Input<Communicator>("Communicator");
    std::string reduction = ctx.Attr<std::string>("reduction");

    auto reduction_op_ = str_to_nccl_red_type(reduction);

    // device id
    int gpu_id = ctx.GetPlace().GetDeviceId();
    int idx = comm->GetCommId(gpu_id);
    VLOG(3) << "gpu : "
            << " invoke allreduce. send " << x->numel() << " recv "
            << out->numel();
    PADDLE_ENFORCE_GPU_SUCCESS(
        phi::dynload::ncclAllReduce(x->data<T>(),
                                    out->mutable_data<T>(ctx.GetPlace()),
                                    out->numel(),
                                    NCCLTypeWrapper<T>::type,
                                    reduction_op_,
                                    comm->comms().at(idx),
                                    ctx.cuda_device_context().stream()));
    VLOG(3) << "gpu : "
            << " finished allreduce. send " << x->numel() << " recv "
            << out->numel();
  }
};

template <typename T, typename DeviceContext>
class NCCLReduceKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    PADDLE_ENFORCE_EQ(
        ctx.GetPlace().GetType() == phi::AllocationType::GPU,
        true,
        phi::errors::InvalidArgument("This kernel only runs on GPU device."));
    auto x = ctx.Input<phi::DenseTensor>("X");  // x0, x1, x2
    auto out = ctx.Output<phi::DenseTensor>("Out");
    auto* comm = ctx.Input<Communicator>("Communicator");
    int root = ctx.Attr<int>("root");
    std::string reduction = ctx.Attr<std::string>("reduction");

    auto reduction_op_ = str_to_nccl_red_type(reduction);

    // device id
    int gpu_id = ctx.GetPlace().GetDeviceId();
    int idx = comm->GetCommId(gpu_id);
    T* recvbuffer = nullptr;
    if (root == gpu_id) {
      recvbuffer = out->mutable_data<T>(ctx.GetPlace());
    } else {
      out->Resize(common::make_ddim({0}));
    }
    VLOG(3) << "gpu : " << gpu_id << " invoke reduce. send " << x->numel()
            << " recv " << out->numel();
    PADDLE_ENFORCE_GPU_SUCCESS(
        phi::dynload::ncclReduce(x->data<T>(),
                                 recvbuffer,
                                 x->numel(),
                                 NCCLTypeWrapper<T>::type,
                                 reduction_op_,
                                 root,
                                 comm->comms().at(idx),
                                 ctx.cuda_device_context().stream()));
    VLOG(3) << "gpu : " << gpu_id << " finished reduce. send " << x->numel()
            << " recv " << out->numel();
  }
};

template <typename T, typename DeviceContext>
class NCCLBcastKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    PADDLE_ENFORCE_EQ(
        ctx.GetPlace().GetType() == phi::AllocationType::GPU,
        true,
        phi::errors::InvalidArgument("This kernel only runs on GPU device."));
    int root = ctx.Attr<int>("root");
    auto* comm = ctx.Input<Communicator>("Communicator");
    // device id
    int gpu_id = ctx.GetPlace().GetDeviceId();
    int idx = comm->GetCommId(gpu_id);
    if (idx == root) {
      auto* x = ctx.Input<phi::DenseTensor>("X");
      VLOG(3) << "gpu : " << gpu_id << " invoke Bcast. send " << x->numel();
      PADDLE_ENFORCE_GPU_SUCCESS(phi::dynload::ncclBcast(
          reinterpret_cast<void*>(const_cast<T*>(x->data<T>())),
          x->numel(),
          NCCLTypeWrapper<T>::type,
          root,
          comm->comms().at(idx),
          ctx.cuda_device_context().stream()));
      VLOG(3) << "gpu : " << gpu_id << " finished Bcast.";
    } else {
      auto* out = ctx.Output<phi::DenseTensor>("Out");
      VLOG(3) << "gpu : " << gpu_id << " invoke Bcast. recv buffer "
              << common::product(out->dims());
      PADDLE_ENFORCE_GPU_SUCCESS(
          phi::dynload::ncclBcast(out->mutable_data<T>(ctx.GetPlace()),
                                  out->numel(),
                                  NCCLTypeWrapper<T>::type,
                                  root,
                                  comm->comms().at(idx),
                                  ctx.cuda_device_context().stream()));
      VLOG(3) << "gpu : " << gpu_id << " finished Bcast. recv " << out->numel();
    }
  }
};

}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;
PD_REGISTER_STRUCT_KERNEL(
    ncclAllReduce, GPU, ALL_LAYOUT, ops::NCCLAllReduceKernel, float) {}
PD_REGISTER_STRUCT_KERNEL(
    ncclBcast, GPU, ALL_LAYOUT, ops::NCCLBcastKernel, float) {}
PD_REGISTER_STRUCT_KERNEL(
    ncclReduce, GPU, ALL_LAYOUT, ops::NCCLReduceKernel, float) {}
