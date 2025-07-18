/* Copyright 2023 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/hlo_fusion_analysis.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "llvm/ADT/STLExtras.h"
#include "xla/codegen/hlo_fusion_spec.h"
#include "xla/codegen/ir_emission_utils.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/utils/hlo_traversal.h"
#include "xla/primitive_util.h"
#include "xla/service/gpu/backend_configs.pb.h"
#include "xla/service/gpu/ir_emission_utils.h"
#include "xla/service/gpu/reduction_utils.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/stream_executor/device_description.h"

namespace xla {
namespace gpu {
namespace {

// Returns true if the fusion output contains non-strided slices only.
bool IsInputFusibleNonStridedSlices(
    const absl::Span<const HloInstructionAdaptor> fusion_roots) {
  return absl::c_all_of(fusion_roots, [&](const HloInstructionAdaptor& root) {
    return IsSliceWithUnitStrides(&root.instruction());
  });
}

// Returns true if all slice inputs in a tuple are equal (ignoring type).
bool AllSliceInputsAreCompatible(
    const absl::Span<const HloInstructionAdaptor> fusion_roots) {
  const Shape& first_slice_operand_shape =
      fusion_roots[0].GetOperand(0).shape();
  return absl::c_all_of(fusion_roots, [&](const HloInstructionAdaptor& slice) {
    return ShapeUtil::EqualIgnoringElementType(slice.GetOperand(0).shape(),
                                               first_slice_operand_shape);
  });
}

// Returns a description of a transpose hero, that is compatible with all roots.
//
// A root is compatible with the transpose hero if:
//   * Either the root has a traspose hero with the same normalized dimensions
//   * Or the root output shape is equal to the the transpose input shape
std::optional<TransposeDescription> FindConsistentTransposeHero(
    const absl::InlinedVector<HloInstructionAdaptor, 2>& hlo_roots,
    const absl::InlinedVector<HloInstructionAdaptor, 2>& heroes) {
  std::optional<TransposeDescription> tiled_transpose_hero;
  std::vector<const HloInstruction*> non_transpose_roots;

  for (auto [root, hero] : llvm::zip(hlo_roots, heroes)) {
    if (auto tr = GetDescriptionForTiledTransposeEmitter(hero.instruction())) {
      if (!tiled_transpose_hero) {
        // First transpose hero found.
        tiled_transpose_hero = tr;
      } else if (!tiled_transpose_hero->IsEquivalent(*tr)) {
        // Transpose heroes have different shape.
        return std::nullopt;
      }
    } else {
      non_transpose_roots.push_back(&root.instruction());
    }
  }

  if (!tiled_transpose_hero) return std::nullopt;

  for (auto* root : non_transpose_roots) {
    // Roots that don't have a transpose hero, should have a shape compatible
    // with the transpose input.
    if (!ShapeUtil::IsReshapeOrTransposeBitcast(
            root->shape(), tiled_transpose_hero->input_shape(),
            /*ignore_element_type=*/true)) {
      return std::nullopt;
    }
  }

  return tiled_transpose_hero;
}

bool UseConcatenateFusion(absl::Span<const HloInstructionAdaptor> roots,
                          absl::Span<const HloInstructionAdaptor> heroes) {
  if (heroes.size() != 1) return false;
  if (heroes.front().opcode() != HloOpcode::kConcatenate) return false;
  // The concat emitter does not support multiple outputs yet. TODO(csigg): fix.
  if (roots.front().shape().IsTuple()) return false;
  // Limit the number of operands because the concat emitter produces code for
  // each operand, hurting occupancy.
  if (heroes.front().instruction().operand_count() > 4) return false;
  // The loop emitter is faster when warp divergence and occupancy are both low.
  // TODO(csigg): exclude this case.
  return true;
}

HloFusionAnalysis::EmitterFusionKind GetEmitterFusionKind(
    const FusionBackendConfig& fusion_backend_config,
    absl::Span<const HloInstructionAdaptor> fusion_roots,
    absl::Span<const HloInstructionAdaptor> fusion_heroes,
    const std::optional<TransposeDescription>& tiled_transpose,
    const se::DeviceDescription& device_info) {
  if (fusion_backend_config.kind() == kCustomFusionKind) {
    return HloFusionAnalysis::EmitterFusionKind::kCustomFusion;
  }

  if (fusion_backend_config.kind() == kTritonFusionKind ||
      fusion_backend_config.kind() == kTritonGemmFusionKind ||
      fusion_backend_config.kind() == kTritonNestedGemmFusionKind) {
    return HloFusionAnalysis::EmitterFusionKind::kTriton;
  }

  if (fusion_backend_config.kind() == kDynamicMemcpyFusionKind) {
    return HloFusionAnalysis::EmitterFusionKind::kDynamicMemcpy;
  }

  if (fusion_backend_config.kind() == kCuDnnFusionKind) {
    return HloFusionAnalysis::EmitterFusionKind::kCuDnn;
  }

  std::optional<HloInstructionAdaptor> first_reduce_hero;
  for (auto [root, hero] : llvm::zip(fusion_roots, fusion_heroes)) {
    if (IsRealReductionHero(root.instruction(), hero.instruction(),
                            device_info)) {
      first_reduce_hero = hero;
      break;
    }
  }
  if (first_reduce_hero.has_value()) {
    bool valid_shapes = true;
    Shape hero_operand_shape = first_reduce_hero->GetOperand(0).shape();
    for (auto [root, hero] : llvm::zip(fusion_roots, fusion_heroes)) {
      if (root == *first_reduce_hero) {
        continue;
      }
      if (!IsRealReductionHero(root.instruction(), hero.instruction(),
                               device_info)) {
        // Needs to have a compatible shape to the reduce operand (compatible
        // meaning same number of elements).
        if (ShapeUtil::ElementsIn(root.shape()) !=
            ShapeUtil::ElementsIn(hero_operand_shape)) {
          valid_shapes = false;
          break;
        }
      } else if (!AreReductionsMultiOutputFusionCompatible(
                     &hero.instruction(), &first_reduce_hero->instruction())) {
        valid_shapes = false;
        break;
      }
    }
    if (valid_shapes) {
      return HloFusionAnalysis::EmitterFusionKind::kReduction;
    }
  }

  // We expect that the last dimension is swapped with a different dimension.
  if (tiled_transpose.has_value()) {
    return HloFusionAnalysis::EmitterFusionKind::kTranspose;
  }

  if (fusion_roots.size() > 1) {
    if (IsInputFusibleNonStridedSlices(fusion_roots) &&
        AllSliceInputsAreCompatible(fusion_roots)) {
      return HloFusionAnalysis::EmitterFusionKind::kInputSlices;
    }
    return HloFusionAnalysis::EmitterFusionKind::kLoop;
  }

  if (fusion_roots[0].opcode() == HloOpcode::kScatter) {
    return HloFusionAnalysis::EmitterFusionKind::kScatter;
  }

  if (UseConcatenateFusion(fusion_roots, fusion_heroes)) {
    return HloFusionAnalysis::EmitterFusionKind::kConcatenate;
  }

  return HloFusionAnalysis::EmitterFusionKind::kLoop;
}

const Shape& GetShape(const HloInstructionAdaptor& adaptor) {
  return adaptor.shape();
}

const Shape& GetShape(const HloInstruction* instruction) {
  return instruction->shape();
}

template <typename Container>
int SmallestBitWidth(const Container& args) {
  int bits = std::numeric_limits<int>::max();
  for (const auto& operand : args) {
    const Shape& shape = GetShape(operand);
    if (!shape.IsArray()) continue;
    bits = std::min(bits, shape.element_type() == PRED
                              ? 8
                              : primitive_util::BitWidth(shape.element_type()));
  }
  return bits;
}

}  // namespace

HloFusionAnalysis::HloFusionAnalysis(
    FusionBackendConfig fusion_backend_config, HloFusionSpec fusion_spec,
    EmitterFusionKind emitter_fusion_kind,
    const se::DeviceDescription* device_info,
    std::optional<TransposeDescription> tiled_transpose,
    HloFusionAnalysis::InputOutputInfo input_output_info)
    : fusion_backend_config_(std::move(fusion_backend_config)),
      fusion_spec_(std::move(fusion_spec)),
      emitter_fusion_kind_(emitter_fusion_kind),
      device_info_(device_info),
      tiled_transpose_(tiled_transpose),
      input_output_info_(std::move(input_output_info)) {}

// static
HloFusionAnalysis HloFusionAnalysis::Create(
    FusionBackendConfig backend_config,
    std::unique_ptr<HloFusionAdaptor> fusion,
    const se::DeviceDescription* device_info) {
  absl::InlinedVector<HloInstructionAdaptor, 2> roots = fusion->GetRoots();
  absl::InlinedVector<HloInstructionAdaptor, 2> heroes;
  for (auto root : roots) {
    heroes.push_back(FindNonTrivialHero(root));
  }

  InputOutputInfo input_output_info{
      /*smallest_input_dtype_bits=*/SmallestBitWidth(fusion->GetParameters()),
      /*smallest_output_dtype_bits=*/SmallestBitWidth(roots),
  };

  std::optional<TransposeDescription> tiled_transpose_hero =
      FindConsistentTransposeHero(roots, heroes);

  EmitterFusionKind emitter_fusion_kind = GetEmitterFusionKind(
      backend_config, roots, heroes, tiled_transpose_hero, *device_info);

  // The loop emitter always uses the roots as heroes. Even if we found some
  // non-trivial heroes in the fusion, some logic in GetEmitterFusionKind might
  // still decide to use the loop emitter and in that case we need to reset the
  // heroes to the roots.
  if (emitter_fusion_kind == EmitterFusionKind::kLoop) {
    heroes = roots;
    tiled_transpose_hero = std::nullopt;
  }

  HloFusionSpec fusion_spec(std::move(fusion), std::move(roots),
                            std::move(heroes));

  return HloFusionAnalysis(std::move(backend_config), std::move(fusion_spec),
                           emitter_fusion_kind, device_info,
                           tiled_transpose_hero, std::move(input_output_info));
}

// static
HloFusionAnalysis HloFusionAnalysis::Create(
    const HloInstruction& instruction,
    const se::DeviceDescription& device_info) {
  absl::StatusOr<GpuBackendConfig> gpu_backend_config =
      instruction.backend_config<GpuBackendConfig>();

  FusionBackendConfig fusion_backend_config =
      gpu_backend_config.ok() ? gpu_backend_config->fusion_backend_config()
                              : FusionBackendConfig::default_instance();
  return Create(std::move(fusion_backend_config),
                HloFusionAdaptor::ForInstruction(&instruction), &device_info);
}

// static
HloFusionAnalysis HloFusionAnalysis::Create(
    const HloInstruction& producer, const HloInstruction& consumer,
    const se::DeviceDescription& device_info) {
  absl::StatusOr<GpuBackendConfig> gpu_backend_config;

  if (consumer.has_backend_config()) {
    gpu_backend_config = consumer.backend_config<GpuBackendConfig>();
  }

  if (!gpu_backend_config.ok() && producer.has_backend_config()) {
    gpu_backend_config = producer.backend_config<GpuBackendConfig>();
  }

  FusionBackendConfig fusion_backend_config =
      gpu_backend_config.ok() ? gpu_backend_config->fusion_backend_config()
                              : FusionBackendConfig::default_instance();

  return HloFusionAnalysis::Create(
      std::move(fusion_backend_config),
      HloFusionAdaptor::ForProducerConsumer(&producer, &consumer),
      &device_info);
}

const HloInstruction* HloFusionAnalysis::FindHeroReduction() const {
  if (emitter_fusion_kind_ != EmitterFusionKind::kReduction) {
    return nullptr;
  }
  const auto& roots = fusion_roots();
  CHECK(!roots.empty());
  // We always use the first reduce root that triggers unnested reduction
  // emitter as the hero reduction, since all the reductions are required to
  // have the same shape and layout as verified by
  // `IsFusedReductionOutputConsistent()`.
  for (auto [root, hero] : llvm::zip(roots, fusion_heroes())) {
    if (IsRealReductionHero(root.instruction(), hero.instruction(),
                            *device_info_)) {
      return &hero.instruction();
    }
  }
  LOG(FATAL) << "Did not find a hero reduction";
}

}  // namespace gpu
}  // namespace xla
