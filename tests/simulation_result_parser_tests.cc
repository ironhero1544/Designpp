// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include "designpp/adapters/simulation_result_parser.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {

TEST_CLASS(SimulationResultParserTests){
  public : TEST_METHOD(ParsesPassFailureErrorAndSkip){
      const auto parsed = adapters::ParseCocotbResults(
          "<?xml version=\"1.0\"?><testsuites><testsuite>"
          "<testcase classname=\"counter\" name=\"increments\" time=\"0.25\"/>"
          "<testcase classname=\"counter\" name=\"wraps\" time=\"0.5\">"
          "<failure>expected &lt;0&gt;</failure></testcase>"
          "<testcase classname=\"counter\" name=\"resets\" time=\"0.1\">"
          "<error>driver error</error></testcase>"
          "<testcase classname=\"counter\" name=\"slow\" time=\"0\">"
          "<skipped/></testcase></testsuite></testsuites>");
Assert::IsTrue(parsed.Ok());
Assert::AreEqual(static_cast<std::uint32_t>(4), parsed.Value().total);
Assert::AreEqual(static_cast<std::uint32_t>(1), parsed.Value().passed);
Assert::AreEqual(static_cast<std::uint32_t>(1), parsed.Value().failed);
Assert::AreEqual(static_cast<std::uint32_t>(1), parsed.Value().errors);
Assert::AreEqual(static_cast<std::uint32_t>(1), parsed.Value().skipped);
Assert::AreEqual(std::string("expected <0>"), parsed.Value().cases[1].detail);
Assert::IsFalse(parsed.Value().Passed());
}  // namespace designpp::tests

TEST_METHOD(RejectsMissingAndMalformedResults) {
  Assert::IsFalse(adapters::ParseCocotbResults("").Ok());
  Assert::IsFalse(adapters::ParseCocotbResults("<testsuite></testsuite>").Ok());
  Assert::IsFalse(
      adapters::ParseCocotbResults("<testcase name=\"broken\" time=\"abc\"/>")
          .Ok());
}
}
;

}  // namespace designpp::tests
