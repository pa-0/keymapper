#pragma once

#include "runtime/KeyEvent.h"
#include "Config.h"
#include "ParseKeySequence.h"
#include <iosfwd>
#include <map>

class ParseConfig {
public:
  Config operator()(std::istream& is);

private:
  struct Command {
    std::string name;
    int index;
    bool mapped;
  };

  struct LogicalKey {
    std::string name;
    KeyCode both;
    KeyCode left;
    KeyCode right;
  };

  using It = std::string::const_iterator;

  [[noreturn]] void error(std::string message);
  void parse_line(It begin, It end);
  void parse_context(It* begin, It end);
  void parse_macro(std::string name, It begin, It end);
  bool parse_logical_key_definition(const std::string& name, It it, It end);
  void parse_mapping(std::string name, It begin, It end);
  std::string parse_command_name(It begin, It end) const;
  void parse_command_and_mapping(It in_begin, It in_end,
                                 It out_begin, It out_end);
  KeySequence parse_input(It begin, It end);
  KeySequence parse_output(It begin, It end);
  std::string preprocess_ident(std::string ident) const;
  std::string preprocess(It begin, It end) const;
  KeyCode add_logical_key(std::string name, KeyCode left, KeyCode right);
  void replace_logical_key(KeyCode both, KeyCode left, KeyCode right);
  Config::Filter read_filter(It* it, It end);
  KeyCode get_key_by_name(std::string_view name) const;
  KeyCode add_terminal_command_action(std::string_view command);

  Config::Context& current_context();
  Command* find_command(const std::string& name);
  void add_command(KeySequence input, std::string name);
  void add_mapping(KeySequence input, KeySequence output);
  void add_mapping(std::string name, KeySequence output);

  int m_line_no{ };
  Config m_config;
  std::vector<Command> m_commands;
  std::map<std::string, std::string> m_macros;
  std::vector<LogicalKey> m_logical_keys;
  ParseKeySequence m_parse_sequence;
};
