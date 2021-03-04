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

using std::optional;
using std::string;

using namespace crispy;
using namespace std::string_view_literals;
using namespace std::string_literals;

TEST_CASE("CLI.simple")
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
    optional<cli::FlagStore> flagsOpt = cli::parse(cmd, args);
    REQUIRE(flagsOpt.has_value());

    cli::FlagStore const& flags = flagsOpt.value();

    CHECK(flags.values.size() == 2);
    CHECK(flags.values.at("contour.capture.logical") == cli::Value{true});
    CHECK(flags.values.at("contour.capture.output") == cli::Value{"out.vt"});
}
