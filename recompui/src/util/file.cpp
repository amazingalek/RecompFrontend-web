#include "file.h"

#include <SDL.h>
#ifndef __EMSCRIPTEN__
#include "nfd.h"
#endif
#include "RmlUi/Core.h"

#include "recompui/program_config.h"

#include <filesystem>
#include <fstream>
#include <iomanip>

#if defined(_WIN32)
#include <Shlobj.h>
#elif defined(__linux__)
#include <unistd.h>
#include <pwd.h>
#elif defined(__APPLE__)
#include "apple/rt64_apple.h"
#elif defined(__EMSCRIPTEN__)
#include <atomic>
#include <cmath>
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

namespace recompui {
#if defined(__EMSCRIPTEN__)
    // A browser has no file dialogs, only the page's file picker (an <input type="file">), and it hands over a file's
    // bytes, not its path. So the main thread, where the page is, opens the picker, writes what is picked to
    // /tmp/picked-<n>/ in the in-memory file system, and wakes the thread that asked, which gets those paths: the same
    // as a native dialog that blocks until it closes. A browser only opens a picker on a click or a key the player
    // has just pressed. If the click on the menu's button no longer counts as one by the time the request reaches the
    // page, the page shows a button of its own for it.
    EM_JS(void, recompui_web_pick_files, (int multiple, const char *folder_ptr, int32_t *state_ptr), {
        const folder = UTF8ToString(folder_ptr), state = state_ptr >> 2;
        const finish = (result) => { Atomics.store(HEAP32, state, result); Atomics.notify(HEAP32, state); };
        const input = document.createElement('input');
        input.type = 'file';
        input.multiple = !!multiple;
        input.addEventListener('cancel', () => finish(2));
        input.addEventListener('change', async () => {
            FS.mkdirTree(folder);
            for (const file of input.files) {
                FS.writeFile(folder + '/' + file.name, new Uint8Array(await file.arrayBuffer()));
            }
            finish(input.files.length > 0 ? 1 : 2);
        });
        if (!navigator.userActivation || navigator.userActivation.isActive) {
            input.click();
            return;
        }
        const prompt = document.createElement('div');
        prompt.style.cssText = 'position:fixed;inset:0;display:flex;gap:1em;align-items:center;justify-content:center;' +
            'background:rgba(0,0,0,0.6);z-index:1000';
        const button = (text, action) => {
            const b = document.createElement('button');
            b.textContent = text;
            b.style.cssText = 'font-size:1.2em;padding:0.6em 1.2em';
            b.onclick = () => { prompt.remove(); action(); };
            prompt.appendChild(b);
        };
        button(multiple ? 'Choose files' : 'Choose a file', () => input.click());
        button('Cancel', () => finish(2));
        document.body.appendChild(prompt);
    });

    static std::list<std::filesystem::path> pick_files(bool multiple) {
        static int pick_count = 0;
        const std::string folder = "/tmp/picked-" + std::to_string(++pick_count);
        std::atomic<int32_t> state{0}; // 1 picked, 2 cancelled
        emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VIII, recompui_web_pick_files, int(multiple), folder.c_str(),
            reinterpret_cast<int32_t *>(&state));
        while (state.load() == 0) {
            emscripten_futex_wait(&state, 0, INFINITY);
        }
        std::list<std::filesystem::path> paths;
        if (state.load() == 1) {
            for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                paths.push_back(entry.path());
            }
        }
        return paths;
    }

    static void perform_file_dialog_operation(const std::function<void(bool, const std::filesystem::path&)>& callback) {
        std::list<std::filesystem::path> paths = pick_files(false);
        callback(!paths.empty(), paths.empty() ? std::filesystem::path{} : paths.front());
        for (const auto& path : paths) {
            std::filesystem::remove(path); // a copy in memory, of a file the callback has read or copied by now
        }
    }

    static void perform_file_dialog_operation_multiple(const std::function<void(bool, const std::list<std::filesystem::path>&)>& callback) {
        std::list<std::filesystem::path> paths = pick_files(true);
        callback(!paths.empty(), paths);
        for (const auto& path : paths) {
            std::filesystem::remove(path);
        }
    }
#else
    static void perform_file_dialog_operation(const std::function<void(bool, const std::filesystem::path&)>& callback) {
        nfdnchar_t* native_path = nullptr;
        nfdresult_t result = NFD_OpenDialogN(&native_path, nullptr, 0, nullptr);

        bool success = (result == NFD_OKAY);
        std::filesystem::path path;

        if (success) {
            path = std::filesystem::path{native_path};
            NFD_FreePathN(native_path);
        }

        callback(success, path);
    }

    static void perform_file_dialog_operation_multiple(const std::function<void(bool, const std::list<std::filesystem::path>&)>& callback) {
        const nfdpathset_t* native_paths = nullptr;
        nfdresult_t result = NFD_OpenDialogMultipleN(&native_paths, nullptr, 0, nullptr);

        bool success = (result == NFD_OKAY);
        std::list<std::filesystem::path> paths;
        nfdpathsetsize_t count = 0;

        if (success) {
            NFD_PathSet_GetCount(native_paths, &count);
            for (nfdpathsetsize_t i = 0; i < count; i++) {
                nfdnchar_t* cur_path = nullptr;
                nfdresult_t cur_result = NFD_PathSet_GetPathN(native_paths, i, &cur_path);
                if (cur_result == NFD_OKAY) {
                    paths.emplace_back(std::filesystem::path{cur_path});
                }
            }
            NFD_PathSet_Free(native_paths);
        }

        callback(success, paths);
    }
#endif

    std::filesystem::path file::get_app_folder_path() {
        // directly check for portable.txt (windows and native linux binary)
        if (std::filesystem::exists("portable.txt")) {
            return std::filesystem::current_path();
        }

#if defined(__APPLE__)
        // Check for portable file in the directory containing the app bundle.
        const auto app_bundle_path = file::apple::get_bundle_directory().parent_path();
        if (std::filesystem::exists(app_bundle_path / "portable.txt")) {
            return app_bundle_path;
        }
#endif

        std::filesystem::path recomp_dir{};

#if defined(_WIN32)
        // Deduce local app data path.
        PWSTR known_path = NULL;
        HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &known_path);
        if (result == S_OK) {
            recomp_dir = std::filesystem::path{known_path} / programconfig::get_program_id();
        }

        CoTaskMemFree(known_path);
#elif defined(__linux__) || defined(__APPLE__) || defined(__EMSCRIPTEN__)
        // In a browser, HOME is /home/web_user, which the page keeps in its IndexedDB.
        // check for APP_FOLDER_PATH env var
        if (getenv("APP_FOLDER_PATH") != nullptr) {
            return std::filesystem::path{getenv("APP_FOLDER_PATH")};
        }

#if defined(__APPLE__)
        const auto supportdir = file::apple::get_application_support_directory();
        if (supportdir) {
            return *supportdir / programconfig::get_program_id();
        }
#endif

        const char *homedir;

        if ((homedir = getenv("HOME")) == nullptr) {
            #if defined(__linux__)
            homedir = getpwuid(getuid())->pw_dir;
            #elif defined(__APPLE__)
                homedir = GetHomeDirectory();
            #endif
        }

        if (homedir != nullptr) {
            recomp_dir = std::filesystem::path{homedir} / (std::u8string{u8".config/"} + programconfig::get_program_id());
        }
#endif

        return recomp_dir;
    }

    std::filesystem::path file::get_program_path() {
#if defined(__APPLE__)
        return file::apple::get_bundle_resource_directory();
#elif defined(__linux__) && defined(RECOMP_FLATPAK)
        return "/app/bin";
#else
        return "";
#endif
    }

    std::filesystem::path file::get_asset_path(const char* asset) {
        return file::get_program_path() / "assets" / asset;
    }

    void file::open_file_dialog(std::function<void(bool success, const std::filesystem::path& path)> callback) {
#ifdef __APPLE__
        file::apple::dispatch_on_ui_thread([callback]() {
            perform_file_dialog_operation(callback);
        });
#else
        perform_file_dialog_operation(callback);
#endif
    }

    void file::open_file_dialog_multiple(std::function<void(bool success, const std::list<std::filesystem::path>& paths)> callback) {
#ifdef __APPLE__
        file::apple::dispatch_on_ui_thread([callback]() {
            perform_file_dialog_operation_multiple(callback);
        });
#else
        perform_file_dialog_operation_multiple(callback);
#endif
    }

    void file::show_error_message_box(const char *title, const char *message) {
#ifdef __APPLE__
    std::string title_copy(title);
    std::string message_copy(message);

    file::apple::dispatch_on_ui_thread([title_copy, message_copy] {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title_copy.c_str(), message_copy.c_str(), nullptr);
    });
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, nullptr);
#endif
    }
}
