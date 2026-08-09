#ifndef DESIGNPP_CORE_FLOW_H_
#define DESIGNPP_CORE_FLOW_H_

#include <string>
#include <string_view>
#include <vector>

namespace designpp::core {

// Identifies a stable stage in the Design++ flow model.
enum class StageId {
  kProjectSetup,
  kRtlInput,
  kLint,
  kSimulation,
  kSynthesis,
  kStaticTimingAnalysis,
  kFloorplan,
  kPlacement,
  kClockTreeSynthesis,
  kRouting,
  kDrcLvs,
  kFinalOutputs,
};

/** Describes one node and its dependencies in a flow definition. */
struct StageDefinition {
  StageId id;
  std::vector<StageId> dependencies;
};

/** Owns an ordered, validated EDA flow graph. */
class FlowDefinition final {
 public:
  // Creates the default digital RTL-to-GDS flow.
  [[nodiscard]] static FlowDefinition CreateDefault();

  // Returns the stages in stable display order.
  [[nodiscard]] const std::vector<StageDefinition>& Stages() const noexcept;

  // Returns graph validation failures.
  [[nodiscard]] std::vector<std::wstring> Validate() const;

 private:
  explicit FlowDefinition(std::vector<StageDefinition> stages);

  std::vector<StageDefinition> stages_;
};

// Returns a localized display name for a stage.
[[nodiscard]] std::wstring_view ToDisplayName(StageId stage) noexcept;

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_FLOW_H_
