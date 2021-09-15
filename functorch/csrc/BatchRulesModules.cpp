// Copyright (c) Facebook, Inc. and its affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <functorch/csrc/BatchRulesHelper.h>
#include <functorch/csrc/PlumbingHelper.h>
#include <functorch/csrc/VmapTransforms.h>
#include <ATen/core/dispatch/Dispatcher.h>

namespace at { namespace functorch {


std::tuple<Tensor,optional<int64_t>>
grid_sample_batch_rule(const Tensor& input, optional<int64_t> input_bdim, const Tensor& grid, optional<int64_t> grid_bdim, int64_t interpolation_mode, int64_t padding_mode, bool align_corners) {
  std::vector<int64_t> input_spec {0, 1, 2, 3};
  std::vector<int64_t> grid_spec = input_spec;
  std::vector<int64_t> out_spec = input_spec;

  std::tuple<Tensor, optional<int64_t>> result;
  if (input_bdim && !grid_bdim) {
    auto new_input = reshape_dim_into(*input_bdim, input_spec[1], input);
    auto out = at::grid_sampler(new_input, grid, interpolation_mode, padding_mode, align_corners);
    out = reshape_dim_outof(out_spec[1], input.sizes()[*input_bdim], out);
    result = std::make_tuple(out, out_spec[1]);
  } else if (!input_bdim && grid_bdim) {
    // grid of N(BH)W2 -> NC(BH)W or grid of N(DH)BW3 -> NC(DH)BW
    auto new_grid = reshape_dim_into(*grid_bdim, grid_spec[1], grid);
    auto out = at::grid_sampler(input, new_grid, interpolation_mode, padding_mode, align_corners);
    out = reshape_dim_outof(out_spec[2], grid.sizes()[*grid_bdim], out);
    result = std::make_tuple(out, out_spec[2]);
  } else if (input_bdim && grid_bdim) {
    auto new_input = reshape_dim_into(*input_bdim, input_spec[0], input);
    auto new_grid = reshape_dim_into(*grid_bdim, grid_spec[0], grid);
    auto out = at::grid_sampler(new_input, new_grid, interpolation_mode, padding_mode, align_corners);
    out = reshape_dim_outof(out_spec[0], input.sizes()[*grid_bdim], out);
    result = std::make_tuple(out, out_spec[0]);
  } else {
    result = std::make_tuple(at::grid_sampler(input, grid, interpolation_mode, padding_mode, align_corners), nullopt);
  }
  return result;
}

// batching rules translated from jax: https://github.com/google/jax/blob/master/jax/_src/lax/lax.py#L3143

// Does not support batch_group_count (needed for convolution backwards)
std::tuple<Tensor,optional<int64_t>>
convolution_batch_rule(const Tensor& lhs, optional<int64_t> lhs_bdim, const Tensor& rhs, optional<int64_t> rhs_bdim, const optional<Tensor>& bias, optional<int64_t> bias_bdim, IntArrayRef stride, IntArrayRef padding, IntArrayRef dilation, bool transposed, IntArrayRef output_padding, int64_t groups) {
  std::vector<int64_t> lhs_spec(stride.size() + 2);
  std::iota(lhs_spec.begin(), lhs_spec.end(), 0);
  std::vector<int64_t> rhs_spec = lhs_spec;
  std::vector<int64_t> out_spec = lhs_spec;

  // If we have a batched bias or weight, we need to perform the computation separately.
  optional<Tensor> unbatched_bias;
  bool separate_bias;
  if ((rhs_bdim && bias) || bias_bdim) {
    TORCH_INTERNAL_ASSERT(bias.has_value());
    unbatched_bias = nullopt;
    separate_bias = true;
  } else {
    unbatched_bias = bias;
    separate_bias = false;
  }
  std::tuple<Tensor, optional<int64_t>> result;
  if (lhs_bdim && !rhs_bdim) {
    auto new_x = reshape_dim_into(*lhs_bdim, lhs_spec[0], lhs);
    auto out = at::convolution(new_x, rhs, unbatched_bias, stride, padding, dilation, transposed, output_padding, groups);
    out = reshape_dim_outof(out_spec[0], lhs.sizes()[*lhs_bdim], out);
    result = std::make_tuple(out, out_spec[0]);
  } else if (!lhs_bdim && rhs_bdim) {
    if (groups == 1) {
      auto new_w = reshape_dim_into(*rhs_bdim, rhs_spec[0], rhs);
      auto out = at::convolution(lhs, new_w, unbatched_bias, stride, padding, dilation, transposed, output_padding, groups);
      out = reshape_dim_outof(out_spec[1], rhs.sizes()[*rhs_bdim], out);
      result = std::make_tuple(out, out_spec[1]);
    } else {
      auto new_w = reshape_dim_outof(rhs_spec[0] + (*rhs_bdim <= rhs_spec[0]), groups, rhs);
      new_w = reshape_dim_into(*rhs_bdim + (rhs_spec[0] < rhs_bdim), rhs_spec[0] + 1, new_w);
      new_w = reshape_dim_into(rhs_spec[0], rhs_spec[0], new_w);
      auto out = at::convolution(lhs, new_w, unbatched_bias, stride, padding, dilation, transposed, output_padding, groups);
      out = reshape_dim_outof(out_spec[1], groups, out);
      out = reshape_dim_outof(out_spec[1] + 1, rhs.sizes()[*rhs_bdim], out);
      out = reshape_dim_into(out_spec[1], out_spec[1] + 1, out);
      result = std::make_tuple(out, out_spec[1]);
    }
  } else if (lhs_bdim && rhs_bdim) {
    auto new_x = reshape_dim_into(*lhs_bdim, lhs_spec[1], lhs);
    groups *= lhs.sizes()[*lhs_bdim];
    auto new_w = reshape_dim_into(*rhs_bdim, rhs_spec[0], rhs);
    auto out = at::convolution(new_x, new_w, unbatched_bias, stride, padding, dilation, transposed, output_padding, groups);
    out = reshape_dim_outof(out_spec[1], lhs.sizes()[*lhs_bdim], out);
    result = std::make_tuple(out, out_spec[1]);
  } else {
    result = std::make_tuple(at::convolution(lhs, rhs, unbatched_bias, stride, padding, dilation, transposed, output_padding, groups), nullopt);
  }
  if (separate_bias) {
    auto A = std::get<0>(result);
    auto A_batch_dim = std::get<1>(result);
    auto B = *bias;
    auto B_batch_dim = bias_bdim;
    A = moveBatchDimToFront(A, A_batch_dim);
    B = moveBatchDimToFront(B, B_batch_dim);
    for (size_t i = 0; i < out_spec.size() - 2; i++) {
      B = B.unsqueeze(-1);
    }
    B = maybePadToLogicalRank(B, B_batch_dim, rankWithoutBatchDim(A, A_batch_dim));

    return std::make_tuple(at::add(A, B), 0);
  } else {
    return result;
  }
}
Tensor convNd_decomp(const Tensor &self, const Tensor &weight, const optional<Tensor>& bias, IntArrayRef stride, IntArrayRef padding, IntArrayRef dilation, int64_t groups) {
  std::vector<int64_t> t(self.dim() - 2, 0);
  IntArrayRef out_padding(t);
  return at::convolution(self, weight, bias, stride, padding, dilation, false, out_padding, groups);
}

// Tensor convNd_transpose_decomp(const Tensor &self, const Tensor &weight, const optional<Tensor>& bias, IntArrayRef stride, IntArrayRef padding, IntArrayRef dilation, int64_t groups) {
//   std::vector<int64_t> t(self.dim() - 2, 0);
//   IntArrayRef out_padding(t);
//   return at::convolution(self, weight, bias, stride, padding, dilation, true, out_padding, groups);
// }

Tensor mkldnn_convolution_decomp(const Tensor &self, const Tensor &weight, const optional<Tensor>& bias, IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups) {
  std::vector<int64_t> t(self.dim() - 2, 0);
  IntArrayRef out_padding(t);
  return at::convolution(self, weight, bias, stride, padding, dilation, false, out_padding, groups);
}

Tensor cudnn_convolution_plumbing(
    const Tensor & self, const Tensor & weight, IntArrayRef padding,
    IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32) {
  auto maybe_layer = maybeCurrentDynamicLayer();
  TORCH_INTERNAL_ASSERT(maybe_layer.has_value());
  int64_t cur_level = maybe_layer->layerId();

  Tensor self_value;
  optional<int64_t> self_bdim;
  std::tie(self_value, self_bdim) = unwrapTensorAtLevel(self, cur_level);
  Tensor weight_value;
  optional<int64_t> weight_bdim;
  std::tie(weight_value, weight_bdim) = unwrapTensorAtLevel(weight, cur_level);

  // conv2d that we have a batch rule for
  if (self.dim() == 4) {
    // Contiguous because usually conv is followed by BN and BN calls .contiguous
    // which can fail due to https://github.com/facebookresearch/functorch/issues/55
    return at::conv2d(self, weight, nullopt, stride, padding, dilation, groups).contiguous();
  }

  static auto op = c10::Dispatcher::singleton()
    .findSchemaOrThrow("aten::cudnn_convolution", "");
  return slow_fallback<Tensor>(op, { self, weight, padding, stride, dilation, groups, benchmark, deterministic, allow_tf32 });
}

bool first_dim_has_size_1(const Tensor& value, int64_t bdim) {
  if (bdim == 0) {
    return value.size(1) == 1;
  }
  return value.size(0) == 1;
}

std::tuple<Tensor,int64_t,Tensor,int64_t> cudnn_conv_per_sample_grad_rule(
    const Tensor& self, optional<int64_t> self_bdim,
    const Tensor& grad_output, optional<int64_t> grad_output_bdim,
    const Tensor& weight, optional<int64_t> weight_bdim,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups, bool benchmark,
    bool deterministic, bool allow_tf32, std::array<bool, 2> output_mask) {
  TORCH_INTERNAL_ASSERT(self_bdim && grad_output_bdim && !weight_bdim);
  // TODO: No clue if this works if the first non-batch dim isn't size 1
  TORCH_INTERNAL_ASSERT(first_dim_has_size_1(self, *self_bdim));
  TORCH_INTERNAL_ASSERT(self.dim() == 5);

  auto bdim_size = self.size(*self_bdim);
  auto self_ = reshape_dim_into(*self_bdim, 0, self);
  auto in_channels = self_.size(1);
  auto grad_output_ = reshape_dim_into(*grad_output_bdim, 0, grad_output);

  auto grad_self = at::cudnn_convolution_backward_input(
      self_.sizes(), grad_output_, weight,
      padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
  grad_self = reshape_dim_outof(0, bdim_size, grad_self);

  // Copied from https://github.com/pytorch/opacus/blob/master/opacus/grad_sample/conv.py
  auto A = at::im2col(self_, {weight.size(2), weight.size(3)}, dilation, padding, stride);
  auto B = grad_output_.reshape({bdim_size, -1, A.size(-1)});
  auto grad_sample = at::einsum("noq,npq->nop", {B, A});
  grad_sample = grad_sample.view({
      bdim_size, groups, -1, groups, in_channels / groups,
      weight.size(2) * weight.size(3) });
  grad_sample = at::einsum("ngrg...->ngr...", {grad_sample});
  grad_sample = grad_sample.reshape(
      {bdim_size, weight.size(0), weight.size(1), weight.size(2), weight.size(3)});

  return std::make_tuple(grad_self, 0, grad_sample, 0);
}

std::tuple<Tensor,Tensor> cudnn_convolution_backward_plumbing(const Tensor & self, const Tensor & grad_output, const Tensor & weight, IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups, bool benchmark, bool deterministic, bool allow_tf32, std::array<bool, 2> output_mask) {
  auto maybe_layer = maybeCurrentDynamicLayer();
  TORCH_INTERNAL_ASSERT(maybe_layer.has_value());
  int64_t cur_level = maybe_layer->layerId();

  Tensor self_value;
  optional<int64_t> self_bdim;
  std::tie(self_value, self_bdim) = unwrapTensorAtLevel(self, cur_level);
  Tensor grad_output_value;
  optional<int64_t> grad_output_bdim;
  std::tie(grad_output_value, grad_output_bdim) = unwrapTensorAtLevel(grad_output, cur_level);
  Tensor weight_value;
  optional<int64_t> weight_bdim;
  std::tie(weight_value, weight_bdim) = unwrapTensorAtLevel(weight, cur_level);

  if (self_bdim.has_value() && self_value.dim() == 5 && first_dim_has_size_1(self_value, *self_bdim) && grad_output_bdim.has_value() && !weight_bdim.has_value()) {
    c10::impl::ExcludeDispatchKeyGuard guard(kBatchedKey);
    auto result = cudnn_conv_per_sample_grad_rule(
        self_value, self_bdim,
        grad_output_value, grad_output_bdim,
        weight_value, weight_bdim,
        padding, stride, dilation, groups,
        benchmark, deterministic, allow_tf32, output_mask);
    return std::make_tuple(
        makeBatched(std::get<0>(result), std::get<1>(result), cur_level),
        makeBatched(std::get<2>(result), std::get<3>(result), cur_level));
  }

  static auto op = c10::Dispatcher::singleton()
    .findSchemaOrThrow("aten::cudnn_convolution_backward", "");
  return slow_fallback<Tensor,Tensor>(op, { self, grad_output, weight, padding, stride, dilation, groups, benchmark, deterministic, allow_tf32, output_mask });
}

// TODO: replace with targetable functionalization
Tensor one_hot_decomposition_hack(const Tensor &self, int64_t num_classes) {
    TORCH_CHECK(self.dtype() == kLong, "one_hot is only applicable to index tensor.");
    auto shape = self.sizes().vec();

    // empty tensor could be converted to one hot representation,
    // but shape inference is not possible.
    if (self.numel() == 0) {
        if (num_classes <= 0) {
            AT_ERROR("Can not infer total number of classes from empty tensor.");
        } else {
            shape.push_back(num_classes);
            return at::empty(shape, self.options());
        }
    }

    TORCH_CHECK(num_classes > 0, "When vmap-ing torch.nn.functional.one_hot, please "
        "provide an explicit positive num_classes argument.");

    // Disabling all of the following checks. This is OK because scatter has checks too.
    // Maybe one_hot should be a primitive wrt autograd so we don't have to deal with this.
    // // non-empty tensor
    // if (self.device().type() != at::kCUDA) {
    //   //for cuda, rely on device assert thrown by scatter
    //   TORCH_CHECK(self.min().item().toLong() >= 0, "Class values must be non-negative.");
    // }
    // if (self.device().type() != at::kCUDA) {
    //   //rely on device asserts from scatter to avoid sync here
    //   TORCH_CHECK(num_classes > self.max().item().toLong(), "Class values must be smaller than num_classes.");
    // }

    shape.push_back(num_classes);
    Tensor ret = at::zeros(shape, self.options());
    return ret.scatter(-1, self.unsqueeze(-1), 1);
}

template <typename A, A a, typename C>
struct UpsampleBackwardBatchRuleHelper;

template <typename F, F Func, typename A, typename B, typename C, typename... T>
struct UpsampleBackwardBatchRuleHelper<F, Func, typelist<A, B, C, T...>> {
  static std::tuple<Tensor,optional<int64_t>> apply(
      const Tensor& grad_output, optional<int64_t> grad_output_bdim,
      optional<IntArrayRef> output_size, IntArrayRef input_size,
      T... extra_args) {
    auto grad_output_ = reshape_dim_into(*grad_output_bdim, 0, grad_output);
    TORCH_INTERNAL_ASSERT(input_size.size() > 0);

    // input_size is wrong so we correct it
    DimVector physical_input_size(input_size.begin(), input_size.end());
    physical_input_size[0] = grad_output_.sizes()[0];

    auto out = Func(
        grad_output_,
        output_size,
        physical_input_size,
        std::forward<T>(extra_args)...);
    return std::make_tuple(reshape_dim_outof(0, grad_output.sizes()[*grad_output_bdim], out), 0);
  }

};

#define UPSAMPLE_BACKWARD(op, overload) VMAP_SUPPORT(#op"."#overload, SINGLE_ARG(\
    UpsampleBackwardBatchRuleHelper<\
      decltype(&ATEN_FN2(op, overload)),\
      &ATEN_FN2(op, overload),\
      c10::guts::function_traits<decltype(ATEN_FN2(op, overload))>::parameter_types>::apply))

#define UPSAMPLE_BATCH(op) \
  EXISTING_BDIM2(op, vec); \
  EXISTING_BDIM(op);

TORCH_LIBRARY_IMPL(aten, FT_BATCHED_KEY, m) {
  VMAP_SUPPORT("convolution", convolution_batch_rule);
  // m.impl("conv_transpose2d", convNd_transpose_decomp);
  m.impl("mkldnn_convolution", mkldnn_convolution_decomp);
  m.impl("cudnn_convolution_backward", cudnn_convolution_backward_plumbing);
  m.impl("cudnn_convolution", cudnn_convolution_plumbing);
  m.impl("conv1d", convNd_decomp);
  m.impl("conv2d", convNd_decomp);
  m.impl("conv3d", convNd_decomp);


  VMAP_SUPPORT("grid_sampler", grid_sample_batch_rule);
  VMAP_SUPPORT("grid_sampler_2d", grid_sample_batch_rule);
  VMAP_SUPPORT("grid_sampler_3d", grid_sample_batch_rule);
  m.impl("cudnn_grid_sampler", cudnn_convolution_plumbing);

  UNARY_POINTWISE(constant_pad_nd);
  EXISTING_BDIM(reflection_pad1d);
  EXISTING_BDIM(reflection_pad2d);
  EXISTING_BDIM(reflection_pad3d);
  EXISTING_BDIM(replication_pad1d);
  EXISTING_BDIM(replication_pad2d);
  EXISTING_BDIM(replication_pad3d);

  UPSAMPLE_BATCH(upsample_bicubic2d);
  UPSAMPLE_BATCH(upsample_bilinear2d);
  UPSAMPLE_BATCH(upsample_linear1d);
  UPSAMPLE_BATCH(upsample_nearest1d);
  UPSAMPLE_BATCH(upsample_nearest2d);
  UPSAMPLE_BATCH(upsample_nearest3d);
  UPSAMPLE_BATCH(upsample_trilinear3d);

  UPSAMPLE_BACKWARD(upsample_bicubic2d_backward, vec);
  UPSAMPLE_BACKWARD(upsample_bilinear2d_backward, vec);
  UPSAMPLE_BACKWARD(upsample_linear1d_backward, vec);
  UPSAMPLE_BACKWARD(upsample_nearest1d_backward, vec);
  UPSAMPLE_BACKWARD(upsample_nearest2d_backward, vec);
  UPSAMPLE_BACKWARD(upsample_nearest3d_backward, vec);
  UPSAMPLE_BACKWARD(upsample_trilinear3d_backward, vec);
  m.impl("one_hot", one_hot_decomposition_hack);
}
}}
