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
#include <contour/CaptureScreen.h>

#include <crispy/utils.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#if defined(_WIN32)
    #include <Winsock2.h>
    #include <Windows.h>
#else
    #include <sys/select.h>
    #include <termios.h>
    #include <unistd.h>
#endif

#if !defined(STDIN_FILENO)
    #define STDIN_FILENO 0
#endif

using std::cerr;
using std::copy_n;
using std::cout;
using std::nullopt;
using std::ofstream;
using std::optional;
using std::stoi;
using std::string;
using std::string_view;
using std::tuple;

using namespace std::string_view_literals;

namespace
{
    void usageText()
    {
        cout <<
            "Usage: contour capture [-t TIMEOUT] [-a] -o FILENAME [COUNT]\n"
            "\n"
            "  -l             Tells the terminal to use logical lines for counting and capturing\n"
            "  -h             Shows this help text.\n"
            "  -o FILENAME    Path to file which will be written with the screen capture.\n"
            "  -t TIMEOUT     Sets timeout seconds to wait for terminal to respond. (default: 1.0)\n"
            "  COUNT          The number of lines to capture.\n"
            "\n"
            ;
    }

    int usageError()
    {
        cerr << "Invalid arguments.\n";
        usageText();
        return EXIT_FAILURE;
    }

    struct Stdin
    {
        bool configured = false;
#if !defined(_WIN32)
        termios savedModes{};
#else
        DWORD savedModes{};
#endif

        ~Stdin()
        {
#if !defined(_WIN32)
            tcsetattr(STDIN_FILENO, TCSANOW, &savedModes);
#else
            auto stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
            SetConsoleMode(stdinHandle, savedModes);
#endif
        }

        Stdin()
        {
#if !defined(_WIN32)
            if (!isatty(STDIN_FILENO))
            {
                std::cerr << "Standard input must be connected to a terminal.\n";
                return;
            }

            if (!isatty(STDOUT_FILENO))
            {
                std::cerr << "Standard output must be connected to a terminal.\n";
                return;
            }

            if (tcgetattr(STDIN_FILENO, &savedModes) < 0)
                return;

            // disable buffered input
            termios ios = savedModes;
            ios.c_lflag &= ~(ICANON | ECHO);
            if (tcsetattr(STDIN_FILENO, TCSANOW, &ios) < 0)
                return;

            configured = true;
#else
            auto stdinHandle = GetStdHandle(STD_INPUT_HANDLE);

            GetConsoleMode(stdinHandle, &savedModes);

            DWORD modes = savedModes;
            modes |= ENABLE_VIRTUAL_TERMINAL_INPUT;
            modes &= ~ENABLE_LINE_INPUT;
            modes &= ~ENABLE_ECHO_INPUT;

            SetConsoleMode(stdinHandle, modes);
            configured = true;
#endif
        }

        int wait(timeval* _timeout)
        {
#if defined(_WIN32)
            auto const fd0 = GetStdHandle(STD_INPUT_HANDLE);
            DWORD const timeoutMillis = _timeout->tv_sec * 1000 + _timeout->tv_usec / 1000;
            DWORD const result = WaitForSingleObject(fd0, timeoutMillis);
            switch (result)
            {
                case WSA_WAIT_EVENT_0:
                    return 1;
                case WSA_WAIT_TIMEOUT:
                    return 0;
                case WAIT_FAILED:
                case WAIT_ABANDONED:
                default:
                    return -1;
            }
#else
            auto const fd0 = STDIN_FILENO;
            fd_set sin, sout, serr;
            FD_ZERO(&sin);
            FD_ZERO(&sout);
            FD_ZERO(&serr);
            FD_SET(fd0, &sin);
            auto const watermark = fd0 + 1;
            return select(watermark, &sin, &sout, &serr, _timeout);
#endif
        }

        int read(void* _buf, size_t _size)
        {
#if defined(_WIN32)
            DWORD nread{};
            if (ReadFile(GetStdHandle(STD_INPUT_HANDLE), _buf, static_cast<DWORD>(_size), &nread, nullptr))
                return static_cast<int>(nread);
            else
                return -1;
#else
            return ::read(STDIN_FILENO, _buf, _size);
#endif
        }

        optional<tuple<int, int>> screenSize(timeval* _timeout)
        {
            // Naive implementation. TODO: use select() to poll and time out properly.
            cout << "\033[18t"; // get line/column count from terminal
            cout.flush();

            if (wait(_timeout) <= 0)
                return nullopt;

            // Consume reply: `CSI 8 ; <LINES> ; <COLUMNS> t`
            string reply;
            for (;;)
            {
                char ch{};
                if (read(&ch, sizeof(ch)) != sizeof(ch))
                    return nullopt;

                if (ch == 't')
                    break;

                reply.push_back(ch);
            }

            auto const screenSizeReply = crispy::split(reply, ';');
            auto const columns = stoi(string(screenSizeReply.at(1)));
            auto const lines = stoi(string(screenSizeReply.at(2)));

            return tuple{columns, lines};
        }
    };

    struct Settings
    {
        bool logicalLines = false;          // -l
        float timeout = 1.0f;               // -t <timeout in seconds>
        string_view outputFile;             // -o <outputfile>
        int verbosityLevel = 0;             // -v, -q (XXX intentionally not parsed currently!)
        int lineCount = 0;                  // (use terminal default)
    };

    optional<Settings> parseCommandLineOptions(int argc, char* argv[])
    {
        try
        {
            auto settings = Settings{};
            int i = 1; // currently parsed CLI argument

            // parse options
            while (i < argc)
            {
                auto const arg = string_view(argv[i]);
                if (arg == "-l"sv)
                    settings.logicalLines = true;
                else if (arg == "-h"sv)
                {
                    usageText();
                    exit(EXIT_SUCCESS);
                }
                else if (arg == "-o"sv)
                {
                    if (!(i + 1 < argc))
                        return nullopt;
                    settings.outputFile = argv[++i];
                }
                else if (arg == "-t"sv)
                {
                    if (!(i + 1 < argc))
                        return nullopt;
                    settings.timeout = std::stof(argv[++i]);
                }
                else
                    break;
                i++;
            }

            if (i < argc)
                settings.lineCount = stoi(argv[i++]);

            if (i < argc)
                return nullopt; // Stray arguments found.

            return settings;
        }
        catch (...)
        {
            return nullopt; // Failures such as from stoi() because of invalid parameters.
        }
    }

    auto constexpr ReplyPrefix = "\033]314;"sv; // DCS 314 ;
    auto constexpr ReplySuffix = "\033\\"sv;    // ST

    bool readCaptureChunk(Stdin& _input, timeval* _timeout, string& _reply)
    {
        timeval timeout = *_timeout;
        // Response is of format: OSC 314 ; <screen capture> ST`
        long long int n = 0;
        while (true)
        {
            int rv = _input.wait(&timeout);
            if (rv < 0)
            {
                perror("select");
                return false;
            }
            else if (rv == 0)
            {
                cerr << "VTE did not respond to CAPTURE `CSI > Ps ; Ps ; Ps t`.\n";
                return false;
            }

            char buf[4096];
            rv = _input.read(buf, sizeof(buf));
            if (rv < 0)
            {
                perror("read");
                return false;
            }
            cerr << fmt::format("Read {} bytes.\n", rv);

            copy_n(buf, rv, back_inserter(_reply));

            if (n == 0 && !crispy::startsWith(string_view(_reply), ReplyPrefix))
            {
                cerr << fmt::format("Invalid respons from terminal received. Does not start with expected reply prefix.\n");
                return false;
            }
            n++;

            if (crispy::endsWith(string_view(_reply), ReplySuffix))
                break;
        }

        return true;
    }

    bool capture(Settings const& _settings)
    {
        auto input = Stdin{};
        if (!input.configured)
            return false;

        auto constexpr MicrosPerSecond = 1000000;
        auto const timeoutMicros = int(_settings.timeout * MicrosPerSecond);
        auto timeout = timeval{};
        timeout.tv_sec = timeoutMicros / MicrosPerSecond;
        timeout.tv_usec = timeoutMicros % MicrosPerSecond;

        auto const screenSizeOpt = input.screenSize(&timeout);
        if (!screenSizeOpt.has_value())
        {
            cerr << "Could not get current screen size.\n";
            return false;
        }
        auto const [numColumns, numLines] = screenSizeOpt.value();

        if (_settings.verbosityLevel > 0)
            cout << fmt::format("Screen size: {}x{}. Capturing lines {} to file {}.\n",
                                numColumns, numLines,
                                _settings.logicalLines ? "logical" : "physical",
                                _settings.lineCount,
                                _settings.outputFile.data());

        cout << fmt::format("\033[>{};{}t",
                            _settings.logicalLines ? '1' : '0',
                            _settings.lineCount);
        cout.flush();

        // request screen capture
        string reply;
        reply.reserve(numColumns * std::max(_settings.lineCount, numLines));

        ofstream output(_settings.outputFile.data());
        while (true)
        {
            if (!readCaptureChunk(input, &timeout, reply))
                return false;

            auto const payload = string_view(reply.data() + ReplyPrefix.size(),
                                             reply.size() - ReplyPrefix.size() - ReplySuffix.size());

            if (payload.empty())
                break;

            output.write(payload.data(), payload.size());
            reply.clear();
        }
        return true;
    }
}

int contour::captureScreenApp(int argc, char* argv[])
{
    optional<Settings> configOpt = parseCommandLineOptions(argc, argv);
    if (!configOpt.has_value())
    {
        usageError();
        return EXIT_FAILURE;
    }
    Settings const& settings = configOpt.value();
    if (settings.outputFile.empty())
    {
        usageError();
        return EXIT_FAILURE;
    }

    if (!capture(settings))
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

