// Copyright 2026 The Design++ Authors

#include <string>

#include "CppUnitTest.h"
#include "designpp/application/wsl_distribution_service.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

std::string EncodeUtf16Le(std::wstring_view text) {
  std::string result;
  result.reserve(text.size() * 2);
  for (wchar_t character : text) {
    result.push_back(static_cast<char>(character & 0xff));
    result.push_back(static_cast<char>((character >> 8) & 0xff));
  }
  return result;
}

}  // namespace

TEST_CLASS(WslDistributionServiceTests){
  public : TEST_METHOD(ParsesUtf16VerboseListAndDefaultDistribution){
      const std::string output = EncodeUtf16Le(
          L"  NAME             STATE           VERSION\r\n"
          L"* Ubuntu-24.04     Running         2\r\n"
          L"  Debian           Stopped         1\r\n");

auto parsed = application::ParseWslDistributionList(output);

Assert::IsTrue(parsed.Ok());
Assert::AreEqual<std::size_t>(2, parsed.Value().size());
Assert::AreEqual("Ubuntu-24.04", parsed.Value()[0].name.c_str());
Assert::IsTrue(parsed.Value()[0].is_default);
Assert::AreEqual<std::uint32_t>(2, parsed.Value()[0].version);
Assert::AreEqual("Debian", parsed.Value()[1].name.c_str());
Assert::IsFalse(parsed.Value()[1].is_default);
Assert::AreEqual<std::uint32_t>(1, parsed.Value()[1].version);
}  // namespace designpp::tests

TEST_METHOD(ParsesLocalizedUtf8HeadersWithoutDependingOnHeaderText) {
  auto parsed = application::ParseWslDistributionList(
      "  이름              상태            버전\r\n"
      "* Ubuntu            실행 중         2\r\n");

  Assert::IsTrue(parsed.Ok());
  Assert::AreEqual<std::size_t>(1, parsed.Value().size());
  Assert::AreEqual("Ubuntu", parsed.Value()[0].name.c_str());
  Assert::AreEqual("실행 중", parsed.Value()[0].state.c_str());
}

TEST_METHOD(RejectsHeaderOnlyAndMalformedUtf16Output) {
  Assert::IsFalse(application::ParseWslDistributionList(
                      EncodeUtf16Le(L"NAME STATE VERSION\r\n"))
                      .Ok());
  Assert::IsFalse(
      application::ParseWslDistributionList(std::string("A\0B", 3)).Ok());
}
}
;

}  // namespace designpp::tests
