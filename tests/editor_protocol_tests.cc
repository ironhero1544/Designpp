// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <string>

#include "designpp/application/editor_protocol.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {

// clang-format cannot parse the CppUnitTest class and method declaration macros.
// clang-format off
TEST_CLASS(EditorProtocolTests) {
 public:
  TEST_METHOD(DecodesSaveWithoutLosingEscapedText) {
    const std::string json =
        R"({"protocol":1,"type":"save_document","session_id":"session","document_id":"doc","version":7,"text":"module top;\n  string s = \"한글\";\nendmodule\n"})";
    auto decoded = application::DecodeEditorWebMessage(json, "session");
    Assert::IsTrue(decoded.Ok());
    Assert::AreEqual(std::string("doc"), decoded.Value().document_id);
    Assert::IsTrue(decoded.Value().text.find("한글") != std::string::npos);
    Assert::IsTrue(decoded.Value().text.find('\n') != std::string::npos);
  }

  TEST_METHOD(RejectsUnknownTypeAndSession) {
    auto unknown = application::DecodeEditorWebMessage(
        R"({"protocol":1,"type":"execute","session_id":"one"})", "one");
    Assert::IsFalse(unknown.Ok());
    auto wrong_session = application::DecodeEditorWebMessage(
        R"({"protocol":1,"type":"ready","session_id":"two"})", "one");
    Assert::IsFalse(wrong_session.Ok());
  }

  TEST_METHOD(IgnoresNestedEnvelopeKeys) {
    const auto decoded = application::DecodeEditorWebMessage(
        R"({"payload":{"session_id":"wrong","protocol":9},"protocol":1,"type":"ready","session_id":"session"})",
        "session");
    Assert::IsTrue(decoded.Ok());
    Assert::AreEqual(std::string("ready"), decoded.Value().type);
  }

  TEST_METHOD(RejectsMalformedPrimitiveValues) {
    const auto decoded = application::DecodeEditorWebMessage(
        R"({"protocol":1junk,"type":"ready","session_id":"session"})",
        "session");
    Assert::IsFalse(decoded.Ok());
  }

  TEST_METHOD(EscapesJsonControlCharacters) {
    const std::string escaped = application::EscapeEditorJson("a\n\"b");
    Assert::AreEqual(std::string("\"a\\n\\\"b\""), escaped);
  }
};
// clang-format on

}  // namespace designpp::tests
