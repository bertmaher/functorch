// Copyright (c) Facebook, Inc. and its affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#pragma once
#include <c10/core/DispatchKey.h>
#include <c10/util/Optional.h>
#include <unordered_map>
#include <mutex>

// Forward declared bc I am lazy
namespace c10 { struct AutogradMetaInterface; }

namespace at {
namespace functorch {

struct TORCH_API DynamicLayer {
  DynamicLayer(DispatchKey key, int64_t layerId, optional<int64_t> batchSize = nullopt): key_(key), layerId_(layerId), batchSize_(batchSize) {}

  DispatchKey key() const { return key_; }
  int64_t layerId() const { return layerId_; }
  int64_t batchSize() const {
    TORCH_INTERNAL_ASSERT(batchSize_);
    return *batchSize_;
  }
 private:
  DispatchKey key_;
  int64_t layerId_;
  optional<int64_t> batchSize_;
};

TORCH_API int64_t initAndPushDynamicLayer(DispatchKey key, optional<int64_t> batch_size = nullopt);
TORCH_API DynamicLayer popDynamicLayerAndDeleteMetadata();
TORCH_API c10::optional<DynamicLayer> maybeCurrentDynamicLayer();
TORCH_API const std::vector<DynamicLayer>& getDynamicLayerStack();
TORCH_API void setDynamicLayerStack(const std::vector<DynamicLayer>& stack);

// NB: Not lock safe, you should only call this from Python where the GIL will
// prevent race conditions.
TORCH_API bool areTransformsActive();

// NB: not lock safe. TODO: does it need a lock?
TORCH_API std::shared_ptr<bool> getLifeHandleForLevel(int64_t level);

}
} // namespace at
