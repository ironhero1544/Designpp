// Copyright 2026 The Design++ Authors

#include "designpp/application/module_scanner.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace designpp::application {
namespace {

std::string RemoveCommentsAndStrings(std::string_view input) {
  enum class State { kCode, kLineComment, kBlockComment, kString };
  State state = State::kCode;
  std::string output(input.size(), ' ');
  for (std::size_t index = 0; index < input.size(); ++index) {
    const char character = input[index];
    const char next = index + 1 < input.size() ? input[index + 1] : '\0';
    if (state == State::kCode && character == '/' && next == '/') {
      state = State::kLineComment;
      ++index;
    } else if (state == State::kCode && character == '/' && next == '*') {
      state = State::kBlockComment;
      ++index;
    } else if (state == State::kCode && character == '"') {
      state = State::kString;
    } else if (state == State::kLineComment && character == '\n') {
      state = State::kCode;
      output[index] = '\n';
    } else if (state == State::kBlockComment && character == '*' &&
               next == '/') {
      state = State::kCode;
      ++index;
    } else if (state == State::kString && character == '\\') {
      ++index;
    } else if (state == State::kString && character == '"') {
      state = State::kCode;
    } else if (state == State::kCode) {
      output[index] = character;
    }
  }
  return output;
}

bool IsIdentifierStart(char character) {
  return std::isalpha(static_cast<unsigned char>(character)) ||
         character == '_' || character == '$';
}

bool IsIdentifier(char character) {
  return std::isalnum(static_cast<unsigned char>(character)) ||
         character == '_' || character == '$';
}

struct Token {
  std::string text;
  std::size_t begin = 0;
  std::size_t end = 0;
};

std::vector<Token> Tokens(std::string_view input) {
  std::vector<Token> tokens;
  for (std::size_t index = 0; index < input.size();) {
    if (!IsIdentifierStart(input[index])) {
      ++index;
      continue;
    }
    const std::size_t start = index++;
    while (index < input.size() && IsIdentifier(input[index])) ++index;
    tokens.push_back(
        {std::string(input.substr(start, index - start)), start, index});
  }
  return tokens;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

std::optional<std::size_t> FindClosingParenthesis(std::string_view text,
                                                  std::size_t opening) {
  int depth = 0;
  for (std::size_t index = opening; index < text.size(); ++index) {
    if (text[index] == '(') {
      ++depth;
    } else if (text[index] == ')' && --depth == 0) {
      return index;
    }
  }
  return std::nullopt;
}

std::vector<std::string_view> SplitTopLevel(std::string_view text) {
  std::vector<std::string_view> values;
  std::size_t start = 0;
  int parentheses = 0;
  int brackets = 0;
  int braces = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    switch (text[index]) {
      case '(':
        ++parentheses;
        break;
      case ')':
        --parentheses;
        break;
      case '[':
        ++brackets;
        break;
      case ']':
        --brackets;
        break;
      case '{':
        ++braces;
        break;
      case '}':
        --braces;
        break;
      case ',':
        if (parentheses == 0 && brackets == 0 && braces == 0) {
          values.push_back(text.substr(start, index - start));
          start = index + 1;
        }
        break;
      default:
        break;
    }
  }
  values.push_back(text.substr(start));
  return values;
}

std::optional<std::string> ParameterDefault(std::string_view declaration,
                                            bool* parameter_context) {
  declaration = Trim(declaration);
  const std::vector<Token> tokens = Tokens(declaration);
  if (tokens.empty()) return std::nullopt;
  if (std::any_of(tokens.begin(), tokens.end(), [](const Token& token) {
        return token.text == "parameter";
      })) {
    *parameter_context = true;
  }
  if (std::any_of(tokens.begin(), tokens.end(), [](const Token& token) {
        return token.text == "localparam" || token.text == "type";
      })) {
    return std::nullopt;
  }
  if (!*parameter_context) return std::nullopt;

  int parentheses = 0;
  int brackets = 0;
  int braces = 0;
  std::size_t equals = std::string_view::npos;
  for (std::size_t index = 0; index < declaration.size(); ++index) {
    const char character = declaration[index];
    if (character == '(') ++parentheses;
    if (character == ')') --parentheses;
    if (character == '[') ++brackets;
    if (character == ']') --brackets;
    if (character == '{') ++braces;
    if (character == '}') --braces;
    if (character == '=' && parentheses == 0 && brackets == 0 && braces == 0) {
      equals = index;
      break;
    }
  }
  if (equals == std::string_view::npos) return std::nullopt;
  const std::string_view left = declaration.substr(0, equals);
  const std::string_view value = Trim(declaration.substr(equals + 1));
  if (value.empty()) return std::nullopt;

  const std::vector<Token> left_tokens = Tokens(left);
  for (auto iterator = left_tokens.rbegin(); iterator != left_tokens.rend();
       ++iterator) {
    int square_depth = 0;
    for (std::size_t index = iterator->end; index < left.size(); ++index) {
      if (left[index] == '[') ++square_depth;
      if (left[index] == ']') --square_depth;
    }
    if (square_depth == 0 && iterator->text != "parameter") {
      return iterator->text + "=" + std::string(value);
    }
  }
  return std::nullopt;
}

std::vector<std::string> FindParameterDefaults(std::string_view source,
                                               std::size_t module_name_end) {
  std::size_t cursor = module_name_end;
  while (cursor < source.size() &&
         std::isspace(static_cast<unsigned char>(source[cursor]))) {
    ++cursor;
  }
  if (cursor >= source.size() || source[cursor] != '#') return {};
  ++cursor;
  while (cursor < source.size() &&
         std::isspace(static_cast<unsigned char>(source[cursor]))) {
    ++cursor;
  }
  if (cursor >= source.size() || source[cursor] != '(') return {};
  const auto closing = FindClosingParenthesis(source, cursor);
  if (!closing) return {};

  std::vector<std::string> defaults;
  bool parameter_context = false;
  const std::string_view declarations =
      source.substr(cursor + 1, *closing - cursor - 1);
  for (const std::string_view declaration : SplitTopLevel(declarations)) {
    auto value = ParameterDefault(declaration, &parameter_context);
    if (value) defaults.push_back(std::move(*value));
  }
  return defaults;
}

}  // namespace

std::vector<ModuleDeclaration> SystemVerilogModuleScanner::FindModulesInText(
    std::string_view input) const {
  std::map<std::string, ModuleDeclaration> modules;
  const std::string source = RemoveCommentsAndStrings(input);
  const std::vector<Token> tokens = Tokens(source);
  for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
    if (tokens[index].text != "module") continue;
    std::size_t name_index = index + 1;
    if (tokens[name_index].text == "automatic" ||
        tokens[name_index].text == "static") {
      ++name_index;
    }
    if (name_index >= tokens.size()) continue;
    const Token& name = tokens[name_index];
    modules.try_emplace(
        name.text,
        ModuleDeclaration{name.text, FindParameterDefaults(source, name.end)});
  }
  std::vector<ModuleDeclaration> result;
  result.reserve(modules.size());
  for (auto& [name, declaration] : modules) {
    result.push_back(std::move(declaration));
  }
  return result;
}

std::vector<ModuleDeclaration> SystemVerilogModuleScanner::FindModules(
    const std::vector<std::filesystem::path>& source_files) const {
  std::map<std::string, ModuleDeclaration> modules;
  for (const std::filesystem::path& path : source_files) {
    std::ifstream input(path, std::ios::binary);
    if (!input) continue;
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    for (ModuleDeclaration& declaration : FindModulesInText(bytes)) {
      modules.try_emplace(declaration.name, std::move(declaration));
    }
  }
  std::vector<ModuleDeclaration> result;
  result.reserve(modules.size());
  for (auto& [name, declaration] : modules) {
    result.push_back(std::move(declaration));
  }
  return result;
}

std::vector<std::string> SystemVerilogModuleScanner::FindModuleNames(
    const std::vector<std::filesystem::path>& source_files) const {
  std::vector<std::string> names;
  for (const ModuleDeclaration& module : FindModules(source_files)) {
    names.push_back(module.name);
  }
  return names;
}

}  // namespace designpp::application
