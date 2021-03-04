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
#include <functional>
#include <map>
#include <optional>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace crispy::cli {

/*
    Grammar
    =======

        CLI     := Command
        Command := NAME Option* Section*
        Option  := NAME [Value]
        Section := NAME Option* [Section]

        Value   := STR | BOOL | FLOAT | INT | UINT
        NAME    := [a-zA-Z_-]+

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

enum class Presence {
    Optional,
    Required,
};

struct Option {
    std::string_view name;
    Value value;
    std::string_view helpText = {};
    Presence presence = Presence::Optional;
};

using OptionList = std::vector<Option>;

struct Command {
    std::string_view name;
    std::string_view helpText = {};
    OptionList options = {};
    std::vector<Command> children = {};
};

using CommandList = std::vector<Command>;

struct FlagStore {
    std::map<Name, Value> values;
    // TODO: do we need any more members?
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

    inline auto peekToken(ParseContext const& _context) -> std::string_view
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
        auto const& current = _context.args.at(_context.pos);

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
        if (text == "yes" || text == "true")
            return Value{true};

        if (text == "no" || text == "false")
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
        // Option  := NAME [Value]
        auto const nameOpt = consumeToken(_context);
        if (!nameOpt.has_value())
            return std::nullopt;

        auto const& optionName = nameOpt.value();

        for (auto const& option : _context.currentCommand.back()->options)
        {
            if (option.name == optionName)
            {
                _context.currentOption.emplace_back(&option);

                // parse the value, if available & required

                if (std::holds_alternative<bool>(_context.currentOption.back()->value))
                {
                    auto const peekedToken = peekToken(_context);

                    // if next token is not a boolean token, we assume TRUE
                    if (!isTrue(peekedToken) && !isFalse(peekedToken))
                        return std::pair{optionName, Value{true}};

                    if (isTrue(peekedToken))
                    {
                        consumeToken(_context);
                        return std::pair{optionName, Value{true}};
                    }

                    if (isFalse(peekedToken))
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
        // Option  := Option*
        while (isOptionName(_context))
        {
            auto optionOpt = parseOption(_context);
            if (!optionOpt.has_value())
                break;

            auto optionPrefix = namePrefix(_context);
            auto& [name, value] = optionOpt.value();

            _context.output.values[optionPrefix + Name(name)] = std::move(value);
        }

        return std::nullopt; // TODO
    }

    inline auto tryLookupCommand(ParseContext const& _context) -> Command const*
    {
        auto const peekedToken = peekToken(_context);

        for (Command const& command : _context.currentCommand.back()->children)
            if (peekedToken == command.name)
                return &command;

        return nullptr; // not found
    }

    inline auto parseCommand(Command const& _command, ParseContext& _context) -> bool
    {
        // Command := NAME Option* Section*

        if (_command.name != peekToken(_context))
            return false;

        consumeToken(_context);
        _context.currentCommand.emplace_back(&_command);

        parseOptionList(_context);

        if (Command const* command = tryLookupCommand(_context))
            parseCommand(*command, _context);

        _context.currentCommand.pop_back(); // TODO: use crispy::finally (it's in the other branch)

        // A command must not leave any trailing tokens at the end of parsing
        return _context.pos == _context.args.size();
    }
} // }}}

std::optional<FlagStore> parse(Command const& _command, StringViewList const& _args)
{
    auto context = detail::ParseContext{ _args };

    if (!detail::parseCommand(_command, context))
        return std::nullopt;

    return std::move(context.output);
}

inline StringViewList stringViewList(int argc, char const* _argv[])
{
    StringViewList output;
    output.resize(argc);

    for (auto const i : times(argc))
        output[i] = _argv[i];

    return output;
}

inline std::optional<FlagStore> parse(Command const& _command, int _argc, char const* _argv[])
{
    return parse(_command, stringViewList(_argc, _argv));
}

}
