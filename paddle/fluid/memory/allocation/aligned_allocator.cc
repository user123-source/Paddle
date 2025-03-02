// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/memory/allocation/aligned_allocator.h"

#include <utility>

#include "paddle/common/macros.h"
#include "paddle/fluid/platform/enforce.h"

REGISTER_FILE_SYMBOLS(aligned_allocator);
namespace paddle {
namespace memory {
namespace allocation {

// For memory address alignment
class AlignedAllocation : public Allocation {
 public:
  AlignedAllocation(DecoratedAllocationPtr underlying_allocation, size_t offset)
      : Allocation(
            reinterpret_cast<uint8_t*>(underlying_allocation->ptr()) + offset,
            underlying_allocation->base_ptr(),
            underlying_allocation->size() - offset,
            underlying_allocation->place()),
        underlying_allocation_(std::move(underlying_allocation)) {}

 private:
  DecoratedAllocationPtr underlying_allocation_;
};

AlignedAllocator::AlignedAllocator(
    std::shared_ptr<Allocator> underlying_allocator, size_t alignment)
    : underlying_allocator_(std::move(underlying_allocator)),
      alignment_(alignment) {
  PADDLE_ENFORCE_GT(
      alignment_,
      0,
      platform::errors::InvalidArgument(
          "Alignment should be larger than 0, but got %d", alignment_));
  if (alignment_ & (alignment_ - 1)) {
    PADDLE_THROW(platform::errors::InvalidArgument(
        "Alignment should be power of 2 (2^N), but got %d", alignment_));
  }
}

bool AlignedAllocator::IsAllocThreadSafe() const {
  return underlying_allocator_->IsAllocThreadSafe();
}

phi::Allocation* AlignedAllocator::AllocateImpl(size_t size) {
  auto raw_allocation = underlying_allocator_->Allocate(size + alignment_);
  size_t offset = AlignedPtrOffset(raw_allocation->ptr(), alignment_);
  auto* p = new AlignedAllocation(
      static_unique_ptr_cast<Allocation>(std::move(raw_allocation)), offset);
  return p;
}

void AlignedAllocator::FreeImpl(phi::Allocation* allocation) {
  delete allocation;
}

}  // namespace allocation
}  // namespace memory
}  // namespace paddle
