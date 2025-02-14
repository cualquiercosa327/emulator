#include "std_include.hpp"

#include <windows_emulator.hpp>
#include <win_x64_gdb_stub_handler.hpp>

#include "object_watching.hpp"

namespace
{
    struct analysis_options
    {
        bool use_gdb{false};
        bool concise_logging{false};
        bool verbose_logging{false};
        bool silent{false};
        std::string registry_path{"./registry"};
        std::string emulation_root{};
        std::set<std::string, std::less<>> modules{};
    };

    void watch_system_objects(windows_emulator& win_emu, const std::set<std::string, std::less<>>& modules,
                              const bool cache_logging)
    {
        (void)win_emu;
        (void)modules;
        (void)cache_logging;

#ifdef OS_WINDOWS
        watch_object(win_emu, modules, *win_emu.current_thread().teb, cache_logging);
        watch_object(win_emu, modules, win_emu.process.peb, cache_logging);
        watch_object(win_emu, modules, emulator_object<KUSER_SHARED_DATA64>{win_emu.emu(), kusd_mmio::address()},
                     cache_logging);

        auto* params_hook = watch_object(win_emu, modules, win_emu.process.process_params, cache_logging);

        win_emu.emu().hook_memory_write(
            win_emu.process.peb.value() + offsetof(PEB64, ProcessParameters), 0x8,
            [&win_emu, cache_logging, params_hook, modules](const uint64_t address, size_t,
                                                            const uint64_t value) mutable {
                const auto target_address = win_emu.process.peb.value() + offsetof(PEB64, ProcessParameters);

                if (address == target_address)
                {
                    const emulator_object<RTL_USER_PROCESS_PARAMETERS64> obj{win_emu.emu(), value};

                    win_emu.emu().delete_hook(params_hook);
                    params_hook = watch_object(win_emu, modules, obj, cache_logging);
                }
            });
#endif
    }

    bool run_emulation(windows_emulator& win_emu, const analysis_options& options)
    {
        try
        {
            if (options.use_gdb)
            {
                const auto* address = "127.0.0.1:28960";
                win_emu.log.print(color::pink, "Waiting for GDB connection on %s...\n", address);

                win_x64_gdb_stub_handler handler{win_emu};
                gdb_stub::run_gdb_stub(network::address{"0.0.0.0:28960", AF_INET}, handler);
            }
            else
            {
                win_emu.start();
            }
        }
        catch (const std::exception& e)
        {
            win_emu.log.print(color::red, "Emulation failed at: 0x%" PRIx64 " - %s\n",
                              win_emu.emu().read_instruction_pointer(), e.what());
            throw;
        }
        catch (...)
        {
            win_emu.log.print(color::red, "Emulation failed at: 0x%" PRIx64 "\n",
                              win_emu.emu().read_instruction_pointer());
            throw;
        }

        const auto exit_status = win_emu.process.exit_status;
        if (!exit_status.has_value())
        {
            win_emu.log.print(color::red, "Emulation terminated without status!\n");
            return false;
        }

        const auto success = *exit_status == STATUS_SUCCESS;
        win_emu.log.print(success ? color::green : color::red, "Emulation terminated with status: %X\n", *exit_status);
        return success;
    }

    std::vector<std::u16string> parse_arguments(const std::span<const std::string_view> args)
    {
        std::vector<std::u16string> wide_args{};
        wide_args.reserve(args.size() - 1);

        for (size_t i = 1; i < args.size(); ++i)
        {
            const auto& arg = args[i];
            wide_args.emplace_back(arg.begin(), arg.end());
        }

        return wide_args;
    }

    bool run(const analysis_options& options, const std::span<const std::string_view> args)
    {
        if (args.empty())
        {
            return false;
        }

        application_settings app_settings{
            .application = args[0],
            .arguments = parse_arguments(args),
        };

        const emulator_settings settings{
            .emulation_root = options.emulation_root,
            .registry_directory = options.registry_path,
            .verbose_calls = options.verbose_logging,
            .disable_logging = options.silent,
            .silent_until_main = options.concise_logging,
            .modules = options.modules,
        };

        windows_emulator win_emu{std::move(app_settings), settings};

        (void)&watch_system_objects;
        watch_system_objects(win_emu, options.modules, options.concise_logging);
        win_emu.buffer_stdout = true;

        if (options.silent)
        {
            win_emu.buffer_stdout = false;
            win_emu.callbacks.on_stdout = [](const std::string_view data) {
                (void)fwrite(data.data(), 1, data.size(), stdout);
            };
        }

        const auto& exe = *win_emu.mod_manager.executable;

        const auto concise_logging = options.concise_logging;

        for (const auto& section : exe.sections)
        {
            if ((section.region.permissions & memory_permission::exec) != memory_permission::exec)
            {
                continue;
            }

            auto read_handler = [&, section, concise_logging](const uint64_t address, size_t, uint64_t) {
                const auto rip = win_emu.emu().read_instruction_pointer();
                if (win_emu.mod_manager.find_by_address(rip) != win_emu.mod_manager.executable)
                {
                    return;
                }

                if (concise_logging)
                {
                    static uint64_t count{0};
                    ++count;
                    if (count > 100 && count % 10000 != 0)
                        return;
                }

                win_emu.log.print(color::green,
                                  "Reading from executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 "\n",
                                  section.name.c_str(), address, rip);
            };

            const auto write_handler = [&, section, concise_logging](const uint64_t address, size_t, uint64_t) {
                const auto rip = win_emu.emu().read_instruction_pointer();
                if (win_emu.mod_manager.find_by_address(rip) != win_emu.mod_manager.executable)
                {
                    return;
                }

                if (concise_logging)
                {
                    static uint64_t count{0};
                    ++count;
                    if (count > 100 && count % 10000 != 0)
                        return;
                }

                win_emu.log.print(color::blue, "Writing to executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 "\n",
                                  section.name.c_str(), address, rip);
            };

            win_emu.emu().hook_memory_read(section.region.start, section.region.length, std::move(read_handler));
            win_emu.emu().hook_memory_write(section.region.start, section.region.length, std::move(write_handler));
        }

        return run_emulation(win_emu, options);
    }

    std::vector<std::string_view> bundle_arguments(const int argc, char** argv)
    {
        std::vector<std::string_view> args{};

        for (int i = 1; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }

        return args;
    }

    analysis_options parse_options(std::vector<std::string_view>& args)
    {
        analysis_options options{};

        while (!args.empty())
        {
            auto arg_it = args.begin();
            const auto& arg = *arg_it;

            if (arg == "-d")
            {
                options.use_gdb = true;
            }
            else if (arg == "-s")
            {
                options.silent = true;
            }
            else if (arg == "-v")
            {
                options.verbose_logging = true;
            }
            else if (arg == "-c")
            {
                options.concise_logging = true;
            }
            else if (arg == "-m")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No module provided after -m");
                }

                arg_it = args.erase(arg_it);
                options.modules.insert(std::string(args[0]));
            }
            else if (arg == "-e")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No emulation root path provided after -e");
                }
                arg_it = args.erase(arg_it);
                options.emulation_root = args[0];
            }
            else if (arg == "-r")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No registry path provided after -r");
                }
                arg_it = args.erase(arg_it);
                options.registry_path = args[0];
            }
            else
            {
                break;
            }

            args.erase(arg_it);
        }

        return options;
    }
}

int main(const int argc, char** argv)
{
    try
    {
        auto args = bundle_arguments(argc, argv);
        const auto options = parse_options(args);

        if (args.empty())
        {
            throw std::runtime_error("Application not specified!");
        }

        bool result{};

        do
        {
            result = run(options, args);
        } while (options.use_gdb);

        return result ? 0 : 1;
    }
    catch (std::exception& e)
    {
        puts(e.what());

#if defined(_WIN32) && 0
        MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
#endif
    }

    return 1;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
    return main(__argc, __argv);
}
#endif
