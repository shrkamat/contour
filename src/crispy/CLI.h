/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <crispy/times.h>
#include <crispy/indexed.h>

#include <array>
#include <cassert>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#if !defined(NDEBUG)
    #include <fmt/format.h>
    #include <iostream>
    #define CLI_DEBUG(that) do { std::cerr << (that) << std::endl; } while (0)
#else
    #define CLI_DEBUG(that) do { } while (0)
#endif


namespace crispy::cli {

/*
    Grammar
    =======

        CLI     := Command
        Command := NAME Option* SubCommand?
        Option  := NAME [Value]
        SubCommand := Command

        Value   := STR | BOOL | FLOAT | INT | UINT
        NAME    := <name without = or leading -'s>

    Examples
    ========

        contour -d '*' capture logical timeout 1.0 output "file.vt"
        contour --debug '*' capture --logical --timeout 1.0 --output "file.vt"
        contour --debug '*' capture --logical --timeout=1.0 --output="file.vt"
        contour --debug '*' capture -l -t 1.0 -o "file.vt"

        capture config "contour.yml" debug "foo,bar,com.*"
        capture --config "contour.yml" --debug "foo,bar,com.*"
        capture --config="contour.yml" --debug="foo,bar,com.*"
*/

using Value = std::variant<int, unsigned int, std::string, double, bool>;
using Name = std::string;

enum class Presence
{
    Optional,
    Required,
};

struct Option
{
    std::string_view name;
    Value value;
    std::string_view helpText = {};
    std::string_view placeholder = {}; // TODO: move right below `Value value{};`
    Presence presence = Presence::Optional;
};

using OptionList = std::vector<Option>;

struct Command
{
    std::string_view name;
    std::string_view helpText = {};
    OptionList options = {};
    std::vector<Command> children = {};

    // TODO: default child command, if children are available AND options is empty.
    //       maybe: std::variant<OptionList, DefaultCommand>;
};

using CommandList = std::vector<Command>;

struct FlagStore
{
    std::map<Name, Value> values;
    // TODO: do we need any more members?

    bool boolean(std::string const& _key) const { return std::get<bool>(values.at(_key)); }
    int integer(std::string const& _key) const { return std::get<int>(values.at(_key)); }
    unsigned uint(std::string const& _key) const { return std::get<unsigned>(values.at(_key)); }
    double real(std::string const& _key) const { return std::get<double>(values.at(_key)); }
    std::string const& str(std::string const& _key) const { return std::get<std::string>(values.at(_key)); }

    template <typename T> T get(std::string const& _key) const { return std::get<T>(values.at(_key)); }
};

using StringViewList = std::vector<std::string_view>;

namespace detail // {{{
{
    struct ParseContext {
        StringViewList const& args;
        size_t pos = 0;

        std::deque<Command const*> currentCommand = {};
        std::deque<Option const*> currentOption = {};

        FlagStore output = {};
    };

    inline auto namePrefix(ParseContext const& _context) -> std::string // {{{
    {
        std::string output;
        for (size_t i = 0; i < _context.currentCommand.size(); ++i) // TODO: use crispy::indexed()
        {
            Command const* v = _context.currentCommand.at(i);
            if (i)
                output += '.';
            output += v->name;
        }

        return output;
    } //  }}}

    inline auto currentToken(ParseContext const& _context) -> std::string_view
    {
        if (_context.pos >= _context.args.size())
            return std::string_view{}; // not enough arguments available

        return _context.args.at(_context.pos);
    }

    inline auto isTrue(std::string_view _token) -> bool
    {
        return _token == "true" || _token == "yes";
    }

    inline auto isFalse(std::string_view _token) -> bool
    {
        return _token == "false" || _token == "no";
    }

    inline auto isOptionName(ParseContext const& _context) -> bool
    {
        auto const current = currentToken(_context);

        for (auto const& option : _context.currentCommand.back()->options)
            if (current == option.name)
                return true;

        return false;
    }

    inline auto isCommandName(ParseContext const& _context) -> bool
    {
        auto const& current = _context.args.at(_context.pos);

        for (auto const& subCommand : _context.currentCommand.back()->children)
            if (current == subCommand.name)
                return true;

        return false;
    }

    inline auto consumeToken(ParseContext& _context) -> std::optional<std::string_view>
    {
        // NAME := <just a name>
        if (_context.pos >= _context.args.size())
            return std::nullopt; // not enough arguments available

        CLI_DEBUG(fmt::format("Consuming token '{}'", currentToken(_context)));
        return _context.args.at(_context.pos++);
    }

    inline auto parseValue(ParseContext& _context) -> std::optional<Value> // {{{
    {
        // Value   := STR | BOOL | FLOAT | INT | UINT
        if (_context.pos >= _context.args.size())
            return std::nullopt; // not enough arguments available

        Value value;
        auto const valueOpt = consumeToken(_context);
        if (!valueOpt.has_value())
            return std::nullopt; // not enough arguments available

        auto const& text = valueOpt.value();

        // BOOL
        if (isTrue(text))
            return Value{true};

        if (isFalse(text))
            return Value{false};

        // FLOAT
        try {
            auto const result = Value{std::stod(std::string(text))}; // TODO: avoid malloc
            if (std::holds_alternative<double>(_context.currentOption.back()->value))
                return result;
            throw std::invalid_argument("");
        }
        catch (...) {}

        // UINT
        try {
            auto const result = std::stoul(std::string(text));
            if (std::holds_alternative<unsigned>(_context.currentOption.back()->value))
                return Value{unsigned(result)};
            if (std::holds_alternative<int>(_context.currentOption.back()->value))
                return Value{int(result)};
            throw std::invalid_argument("");
        }
        catch (...) {}

        // INT
        try {
            auto const result = std::stoi(std::string(text));
            if (std::holds_alternative<int>(_context.currentOption.back()->value))
                return Value{int(result)};
            throw std::invalid_argument("");
        }
        catch (...) {}

        // STR
        return Value{std::string(text)};
    } // }}}

    inline auto parseOption(ParseContext& _context) -> std::optional<std::pair<std::string_view, Value>> // {{{
    {
        CLI_DEBUG(fmt::format("parseOption {}", currentToken(_context)));
        // Option  := NAME [Value]
        auto const optionName = currentToken(_context);

        for (auto const& option : _context.currentCommand.back()->options)
        {
            if (optionName == option.name)
            {
                consumeToken(_context);
                _context.currentOption.emplace_back(&option);

                // parse the value, if available & required

                if (std::holds_alternative<bool>(_context.currentOption.back()->value))
                {
                    auto const token = currentToken(_context);

                    // if next token is not a boolean token, we assume TRUE
                    if (!isTrue(token) && !isFalse(token))
                        return std::pair{optionName, Value{true}};

                    if (isTrue(token))
                    {
                        consumeToken(_context);
                        return std::pair{optionName, Value{true}};
                    }

                    if (isFalse(token))
                    {
                        consumeToken(_context);
                        return std::pair{optionName, Value{false}};
                    }
                }
                else
                {
                    // we definitely need a value parameter
                    auto const valueOpt = parseValue(_context);
                    if (!valueOpt.has_value())
                        return std::nullopt;

                    return std::pair{optionName, valueOpt.value()};
                }

                _context.currentOption.pop_back(); // TODO: use crispy::finally()
            }
        }

        return std::nullopt; // this is no option
    } // }}}

    inline auto parseOptionList(ParseContext& _context) -> std::optional<OptionList>
    {
        // Option := Option*

        auto const setOption = [&](std::string const& _key, Value _value) -> void
        {
            CLI_DEBUG(fmt::format("setOption({}): {}", _key, _value));
            _context.output.values[_key] = std::move(_value);
        };

        auto const optionPrefix = namePrefix(_context);

        // pre-fill defaults
        for (Option const& option : _context.currentCommand.back()->options)
        {
            auto const fqdn = optionPrefix + "." + Name(option.name);
            _context.output.values[fqdn] = option.value;
            setOption(fqdn, option.value);
        }

        // consume options
        while (isOptionName(_context))
        {
            auto optionOpt = parseOption(_context);
            if (!optionOpt.has_value())
                break;

            auto& [name, value] = optionOpt.value();
            auto const fqdn = optionPrefix + "." + Name(name);

            CLI_DEBUG(fmt::format("option: {}", fqdn));
            if (std::holds_alternative<bool>(value))
                CLI_DEBUG(fmt::format("    -> (bool) {}", std::get<bool>(value)));
            else if (std::holds_alternative<int>(value))
                CLI_DEBUG(fmt::format("    -> (int) {}", std::get<int>(value)));
            else if (std::holds_alternative<unsigned>(value))
                CLI_DEBUG(fmt::format("    -> (int) {}", std::get<unsigned>(value)));
            else if (std::holds_alternative<double>(value))
                CLI_DEBUG(fmt::format("    -> (double) {}", std::get<double>(value)));
            else if (std::holds_alternative<std::string>(value))
                CLI_DEBUG(fmt::format("    -> (int) {}", std::get<std::string>(value)));
            else
                CLI_DEBUG(fmt::format("    -> (?) ?"));

            setOption(fqdn, std::move(value));
        }

        return std::nullopt; // TODO
    }

    inline auto tryLookupCommand(ParseContext const& _context) -> Command const*
    {
        auto const token = currentToken(_context);

        for (Command const& command : _context.currentCommand.back()->children)
            if (token == command.name)
                return &command;

        return nullptr; // not found
    }

    inline auto parseCommand(Command const& _command, ParseContext& _context) -> bool
    {
        // Command := NAME Option* Section*
        consumeToken(_context); // Name was already ensured to be right (or is assumed to be right).
        _context.currentCommand.emplace_back(&_command);

        parseOptionList(_context);

        if (Command const* command = tryLookupCommand(_context))
            parseCommand(*command, _context);

        _context.currentCommand.pop_back(); // TODO: use crispy::finally (it's in the other branch)

        // A command must not leave any trailing tokens at the end of parsing
        return _context.pos == _context.args.size();
    }
} // }}}

void validate(Command const& _command)
{
    // TODO: throw if Command is not well defined.
    //
    // - no duplicated nems in same scope
    // - names must not start with '-' (dash)
    // - must not contain '='
}

std::optional<FlagStore> parse(Command const& _command, StringViewList const& _args)
{
    validate(_command);

    auto context = detail::ParseContext{ _args };

    // XXX do not enforce checking the first token, as for main()'s argv[0] this most likely is different.
    // if (currentToken(context) != _command.name)
    //     return std::nullopt;

    if (!detail::parseCommand(_command, context))
        return std::nullopt;

    return std::move(context.output);
}

inline StringViewList stringViewList(int argc, char const * const * _argv)
{
    StringViewList output;
    output.resize(argc);

    for (auto const i : times(argc))
        output[i] = _argv[i];

    return output;
}

inline std::optional<FlagStore> parse(Command const& _command, int _argc, char const* const* _argv)
{
    return parse(_command, stringViewList(_argc, _argv));
}

/**
 * Constructs a usage text suitable for printing out the command usage syntax in terminals.
 *
 * @param _command    The command to construct the usage text for.
 * @param colored     Boolean indicating whether or not to colorize the output via VT sequences.
 * @param _textWidth  Number of characters to write at most per line.
 */
std::string usageText(Command const& _command, bool colored, int _textWidth, std::string const& _cmdPrefix = "")
{
    if (_command.children.empty())
    {
        std::stringstream sstr;
        sstr << _cmdPrefix << _command.name;
        for (Option const& option : _command.options)
        {
            if (std::holds_alternative<bool>(option.value))
                sstr << ' ' << "[" << option.name << "]";
            else if (std::holds_alternative<int>(option.value))
                sstr << ' ' << option.name << ' ' << "INT";
            else if (std::holds_alternative<unsigned int>(option.value))
                sstr << ' ' << option.name << ' ' << "UINT";
            else if (std::holds_alternative<double>(option.value))
                sstr << ' ' << option.name << ' ' << "FLOAT";
        }
        sstr << '\n';
        return sstr.str();
    }
    else
    {
        /*
         * a
         * a b
         * a b 1
         * a b 2
         * a b 3 u
         * a b 3 v A
         * a b 3 v B
         *
         */

        std::stringstream prefix;
        prefix << _cmdPrefix << _command.name << ' ';
        for (Option const& option : _command.options)
        {
            if (std::holds_alternative<bool>(option.value))
                prefix << "[" << option.name << "]" << ' ';
            else if (std::holds_alternative<int>(option.value))
                prefix << option.name << ' ' << "INT" << ' ';
            else if (std::holds_alternative<unsigned int>(option.value))
                prefix << option.name << ' ' << "UINT" << ' ';
            else if (std::holds_alternative<double>(option.value))
                prefix << option.name << ' ' << "FLOAT" << ' ';
        }
        std::string const prefixStr = prefix.str();

        std::stringstream sstr;
        for (Command const& subcmd : _command.children)
            sstr << usageText(subcmd, colored, _textWidth, prefixStr);
        return sstr.str();
    }
}

/**
 * Constructs a help text suitable for printing out the command usage syntax in terminals.
 *
 * @param _command    The command to construct the usage text for.
 * @param colored     Boolean indicating whether or not to colorize the output via VT sequences.
 * @param _textWidth  Number of characters to write at most per line.
 */
std::string helpText(Command const& _command, bool colored, int _textWidth)
{
    // TODO: if sub commands are available, list each seperately
    // TODO: also print each option's default, if available
    return ""; // TODO
}

}
