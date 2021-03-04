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
#include <crispy/CLI.h>
#include <catch2/catch.hpp>

// TODO:
//
// - [ ] variations of option names and value attachements
//       all of: NAME [VALUE] | --NAME [VALUE] | -NAME [VALUE] | --NAME[=VALUE]
// - [ ] help output printing (colored, non-colored)
// - [ ] help output auto-detecting screen width, via: VT seq, ioctl(TIOCGWINSZ), manual
// - [ ] presense optional vs presense required
// - [x] test option type: BOOL
// - [ ] test option type: INT
// - [ ] test option type: UINT
// - [ ] test option type: FLOAT (also being passed as INT positive / negative)
// - [ ] test option type: STR (can be any arbitrary string)
// - [ ] test option defaults
// - [ ] CONSIDER: supporting positional arguments (free sanding values of single given type)
//

using std::optional;
using std::string;

namespace cli = crispy::cli;

using namespace std::string_view_literals;
using namespace std::string_literals;

namespace std { // {{{ little Catch2 debug-print helper
    std::ostream& operator<<(std::ostream& os, cli::Value const& _value)
    {
        if (std::holds_alternative<bool>(_value))
            os << fmt::format("(bool) {}", std::get<bool>(_value));
        else if (std::holds_alternative<int>(_value))
            os << fmt::format("(int) {}", std::get<int>(_value));
        else if (std::holds_alternative<unsigned>(_value))
            os << fmt::format("(int) {}", std::get<unsigned>(_value));
        else if (std::holds_alternative<double>(_value))
            os << fmt::format("(double) {}", std::get<double>(_value));
        else if (std::holds_alternative<std::string>(_value))
            os << fmt::format("(string) \"{}\"", std::get<std::string>(_value));
        else
            os << fmt::format("(?) ?");
        return os;
    }
} // }}}

TEST_CASE("CLI.option.type.bool")
{
    auto const cmd = cli::Command{
        "contour",
        "help here",
        cli::OptionList{
            cli::Option{"verbose"sv, cli::Value{false}, "Help text here"sv}
        },
    };

    SECTION("set") {
        auto const args = cli::StringViewList{"contour", "verbose"};
        optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value{true});
    }

    SECTION("set true") {
        auto const args = cli::StringViewList{"contour", "verbose", "true"};
        optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value{true});
    }

    SECTION("set true") {
        auto const args = cli::StringViewList{"contour", "verbose", "false"};
        optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value{false});
    }

    SECTION("unset") {
        auto const args = cli::StringViewList{"contour"};
        optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
        REQUIRE(flagsOpt.has_value());
        CHECK(flagsOpt.value().values.at("contour.verbose") == cli::Value{false});
    }
}

TEST_CASE("CLI.contour-full-test")
{
    auto const cmd = cli::Command{
        "contour",
        "help here",
        cli::OptionList{
            cli::Option{"debug"sv, cli::Value{""s}, "Help text here"sv},
            cli::Option{"config", cli::Value{"~/.config/contour/contour.yml"}, "Help text there"sv},
            cli::Option{"profile", cli::Value{""}, "Help text over here"sv}
        },
        cli::CommandList{
            cli::Command{
                "capture",
                "some capture help text",
                {
                    cli::Option{"logical", cli::Value{false}, "help there"},
                    cli::Option{"timeout", cli::Value{1.0}, "help here"},
                    cli::Option{"output", cli::Value{""}},
                }
            }
        }
    };

    auto const args = cli::StringViewList{"contour", "capture", "logical", "output", "out.vt"};
    optional<cli::FlagStore> const flagsOpt = cli::parse(cmd, args);
    REQUIRE(flagsOpt.has_value());

    cli::FlagStore const& flags = flagsOpt.value();

    CHECK(flags.values.size() == 6);
    CHECK(flags.values.at("contour.debug") == cli::Value{""s});
    CHECK(flags.values.at("contour.config") == cli::Value{"~/.config/contour/contour.yml"});
    CHECK(flags.values.at("contour.profile") == cli::Value{""s});
    CHECK(flags.values.at("contour.capture.logical") == cli::Value{true});
    CHECK(flags.values.at("contour.capture.output") == cli::Value{"out.vt"});
    CHECK(flags.values.at("contour.capture.timeout") == cli::Value{1.0});
}
