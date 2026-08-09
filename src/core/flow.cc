#include "designpp/core/flow.h"

#include <algorithm>
#include <unordered_set>

namespace designpp::core {

FlowDefinition::FlowDefinition(std::vector<StageDefinition> stages)
    : stages_(std::move(stages)) {}

FlowDefinition FlowDefinition::CreateDefault() {
  return FlowDefinition({
      {StageId::kProjectSetup, {}},
      {StageId::kRtlInput, {StageId::kProjectSetup}},
      {StageId::kLint, {StageId::kRtlInput}},
      {StageId::kSimulation, {StageId::kLint}},
      {StageId::kSynthesis, {StageId::kLint}},
      {StageId::kStaticTimingAnalysis, {StageId::kSynthesis}},
      {StageId::kFloorplan, {StageId::kSynthesis}},
      {StageId::kPlacement, {StageId::kFloorplan}},
      {StageId::kClockTreeSynthesis, {StageId::kPlacement}},
      {StageId::kRouting, {StageId::kClockTreeSynthesis}},
      {StageId::kDrcLvs, {StageId::kRouting}},
      {StageId::kFinalOutputs,
       {StageId::kDrcLvs, StageId::kStaticTimingAnalysis}},
  });
}

const std::vector<StageDefinition>& FlowDefinition::Stages() const noexcept {
  return stages_;
}

std::vector<std::wstring> FlowDefinition::Validate() const {
  std::vector<std::wstring> errors;
  std::unordered_set<StageId> seen;

  for (const auto& stage : stages_) {
    if (!seen.insert(stage.id).second) {
      errors.emplace_back(L"플로우에 중복 단계가 있습니다.");
    }
    for (const auto dependency : stage.dependencies) {
      if (!seen.contains(dependency)) {
        errors.emplace_back(std::wstring(ToDisplayName(stage.id)) +
                            L" 단계의 선행 단계가 앞에 정의되지 않았습니다.");
      }
    }
  }

  return errors;
}

std::wstring_view ToDisplayName(const StageId stage) noexcept {
  switch (stage) {
    case StageId::kProjectSetup:
      return L"프로젝트 설정";
    case StageId::kRtlInput:
      return L"RTL 입력";
    case StageId::kLint:
      return L"Lint";
    case StageId::kSimulation:
      return L"시뮬레이션";
    case StageId::kSynthesis:
      return L"합성";
    case StageId::kStaticTimingAnalysis:
      return L"STA";
    case StageId::kFloorplan:
      return L"Floorplan";
    case StageId::kPlacement:
      return L"Placement";
    case StageId::kClockTreeSynthesis:
      return L"CTS";
    case StageId::kRouting:
      return L"Routing";
    case StageId::kDrcLvs:
      return L"DRC / LVS";
    case StageId::kFinalOutputs:
      return L"GDSII / 리포트";
  }
  return L"알 수 없음";
}

}  // namespace designpp::core
