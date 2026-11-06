#include "run_flags.h"
#include "host_path.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    namespace fs = std::filesystem;
    using namespace launcher_flags;
    const fs::path path = fs::temp_directory_path() /
        ("m2-launcher-settings-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".cfg");
    const fs::path host_tree = fs::temp_directory_path() /
        ("m2-launcher-host-path-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto fail = [&](const char* message) {
        std::fprintf(stderr, "%s\n", message);
        fs::remove(path);
        std::error_code ignored;
        fs::remove_all(host_tree, ignored);
        return 1;
    };
    // The launcher must find the host in CMake's split build layout. Installed
    // trees keep it beside the launcher; single-config builds put it in the
    // sibling host/ directory, and multi-config builds add a config directory.
    {
        std::error_code ec;
        fs::remove_all(host_tree, ec);
        const fs::path exe = launcher_host::executable_name();
        const auto touch = [](const fs::path& file) {
            std::error_code create_ec;
            fs::create_directories(file.parent_path(), create_ec);
            std::ofstream(file).close();
        };
        touch(host_tree / "launcher" / "m2launcher");
        touch(host_tree / "host" / exe);
        const fs::path launcher_dir = host_tree / "launcher";
        if (launcher_host::find_host_executable(launcher_dir) != host_tree / "host" / exe)
            return fail("launcher did not find the host in the sibling build directory");
        // SDL reports the base path with a trailing separator.
        if (launcher_host::find_host_executable(launcher_dir / "") != host_tree / "host" / exe)
            return fail("launcher did not handle SDL's trailing base-path separator");
        // A host beside the launcher (installed layout) takes precedence.
        touch(launcher_dir / exe);
        if (launcher_host::find_host_executable(launcher_dir) != launcher_dir / exe)
            return fail("launcher did not prefer the host beside it");
        // Multi-config generators: launcher/Release and host/Release.
        touch(host_tree / "multi" / "launcher" / "Release" / "m2launcher");
        touch(host_tree / "multi" / "host" / "Release" / exe);
        if (launcher_host::find_host_executable(host_tree / "multi" / "launcher" / "Release") !=
            host_tree / "multi" / "host" / "Release" / exe)
            return fail("launcher did not find the host in a multi-config build directory");
#ifdef __APPLE__
        // The staged app bundle lives in build/launcher; CMake copies the host
        // into Contents/MacOS and the build tree keeps it in build/host.
        const fs::path bundle = host_tree / "bundle" / "launcher" /
                                "IMSA WCR M2 Recomp Launcher.app";
        touch(bundle / "Contents" / "MacOS" / exe);
        if (launcher_host::find_host_executable(bundle / "Contents" / "Resources") !=
            bundle / "Contents" / "MacOS" / exe)
            return fail("launcher did not find the host inside the app bundle");
        fs::remove(bundle / "Contents" / "MacOS" / exe, ec);
        touch(host_tree / "bundle" / "host" / exe);
        if (launcher_host::find_host_executable(bundle / "Contents" / "Resources") !=
            host_tree / "bundle" / "host" / exe)
            return fail("launcher did not find the host beside the app bundle");
#endif
        // A launcher built without game data must not settle for a stub host:
        // given a probe, it takes a game-capable host from a sibling build
        // directory of the same source tree, and keeps the usual host when
        // none of them carries the game.
        {
            const fs::path tree = host_tree / "split";
            touch(tree / "build" / "CMakeCache.txt");
            touch(tree / "build" / "launcher" / "m2launcher");
            touch(tree / "build" / "host" / exe);
            touch(tree / "build-game" / "CMakeCache.txt");
            touch(tree / "build-game" / "host" / exe);
            touch(tree / "not-a-build" / "host" / exe);  // no CMakeCache.txt
            const fs::path stub = tree / "build" / "host" / exe;
            const fs::path game = tree / "build-game" / "host" / exe;
            const fs::path launcher_dir = tree / "build" / "launcher";
            const auto only_game = [&](const fs::path& host) { return host == game; };
            if (launcher_host::find_host_executable(launcher_dir, only_game) != game)
                return fail("launcher did not prefer a game-capable sibling build host");
            const auto none = [](const fs::path&) { return false; };
            if (launcher_host::find_host_executable(launcher_dir, none) != stub)
                return fail("launcher did not keep the usual host when none has the game");
            const auto any = [](const fs::path&) { return true; };
            if (launcher_host::find_host_executable(launcher_dir, any) != stub)
                return fail("launcher did not keep the usual host when it has the game");
            const auto only_unrelated = [&](const fs::path& host) {
                return host == tree / "not-a-build" / "host" / exe;
            };
            if (launcher_host::find_host_executable(launcher_dir, only_unrelated) != stub)
                return fail("launcher considered a directory that is not a build tree");
            // Without a probe nothing outside the usual locations is used.
            if (launcher_host::find_host_executable(launcher_dir) != stub)
                return fail("launcher searched sibling builds without a probe");
        }
        fs::remove_all(host_tree, ec);
    }
    // Graphics-tab mapping for the M2_ANISOTROPY catalog row: 0 is off
    // (unset/1x), 1-4 are the sampler levels, and any other hand-typed value
    // must be reported as custom rather than silently snapped to a level.
    if (anisotropy_level_index("") != 0 || anisotropy_level_index("1") != 0 ||
        anisotropy_level_index("2") != 1 || anisotropy_level_index("4") != 2 ||
        anisotropy_level_index("8") != 3 ||
        anisotropy_level_index("16") != 4 ||
        anisotropy_level_index("3") != -1 ||
        anisotropy_level_index("16x") != -1)
        return fail("anisotropy level mapping changed");
    // The same mapping backs the M2_ANTIALIASING combo.
    if (antialiasing_level_index("") != 0 || antialiasing_level_index("1") != 0 ||
        antialiasing_level_index("2") != 1 || antialiasing_level_index("4") != 2 ||
        antialiasing_level_index("8") != 3 ||
        antialiasing_level_index("3") != -1 ||
        antialiasing_level_index("4x") != -1)
        return fail("antialiasing level mapping changed");
    // M2_RENDER_SCALE: index 0 (unset or 0) follows the window; 1x is a real
    // choice, so it must not collapse into index 0.
    if (render_scale_level_index("") != 0 || render_scale_level_index("0") != 0 ||
        render_scale_level_index("1") != 1 || render_scale_level_index("2") != 2 ||
        render_scale_level_index("4") != 4 || render_scale_level_index("8") != 6 ||
        render_scale_level_index("5") != -1 || render_scale_level_index("2x") != -1)
        return fail("render scale level mapping changed");
    // String filters: unset is the original sampling; unknown values are custom.
    if (choice_value_index("", kTextureFilterValues, kTextureFilterCount) != 0 ||
        choice_value_index("linear", kTextureFilterValues, kTextureFilterCount) != 1 ||
        choice_value_index("sharp", kTextureFilterValues, kTextureFilterCount) != 2 ||
        choice_value_index("cubic", kTextureFilterValues, kTextureFilterCount) != -1 ||
        choice_value_index("sharp", kBitmapFilterValues, kBitmapFilterCount) != 2)
        return fail("filter choice mapping changed");
    // Combined Texture filtering ladder: Original, Bilinear, Anisotropic
    // 2x-16x. Sharp textures read as Bilinear, anisotropy wins over the base
    // filter, and unknown values are custom.
    if (texture_filtering_index("", "") != 0 ||
        texture_filtering_index("", "1") != 0 ||
        texture_filtering_index("linear", "") != 1 ||
        texture_filtering_index("sharp", "") != 1 ||
        texture_filtering_index("linear", "2") != 2 ||
        texture_filtering_index("", "4") != 3 ||
        texture_filtering_index("sharp", "8") != 4 ||
        texture_filtering_index("linear", "16") != 5 ||
        texture_filtering_index("cubic", "") != -1 ||
        texture_filtering_index("linear", "3") != -1)
        return fail("texture filtering ladder mapping changed");
    for (int i = 0; i < kTextureFilteringChoiceCount; ++i) {
        const auto& c = kTextureFilteringChoices[i];
        const std::string aniso = c.anisotropy > 1 ? std::to_string(c.anisotropy) : "";
        if (texture_filtering_index(c.filter, aniso.c_str()) != i)
            return fail("texture filtering choice does not round-trip");
    }
    // Enhancements mapping for M2_PLAYER_CAR_VARIANT: 0/1/2 are the
    // authored appearance variants, and any other saved value must be shown as
    // custom rather than snapped to a variant.
    if (car_variant_level_index("") != 0 || car_variant_level_index("0") != 0 ||
        car_variant_level_index("1") != 1 || car_variant_level_index("2") != 2 ||
        car_variant_level_index("3") != -1 ||
        car_variant_level_index("2x") != -1)
        return fail("car variant level mapping changed");
    // Resolution presets must all keep the game's 4:3 (the guest display is
    // 640x480), or the launcher would offer a stretched size. Unknown saved
    // sizes must stay custom rather than snap to a preset.
    for (int i = 0; i < kResolutionPresetCount; ++i) {
        if (kResolutionPresets[i].width * 3 != kResolutionPresets[i].height * 4)
            return fail("a resolution preset is not 4:3");
        if (resolution_index(kResolutionPresets[i].width,
                             kResolutionPresets[i].height) != i)
            return fail("resolution preset lookup changed");
    }
    if (resolution_index(960, 720) < 0 || resolution_index(1280, 960) < 0 ||
        resolution_index(1920, 1080) != -1 || resolution_index(0, 0) != -1)
        return fail("resolution preset mapping changed");
    {
        // Controls tab: the launcher can only display, bind, or remove an
        // action that appears in a section, so a section that drops or
        // duplicates an action would silently hide its binding. The fourteen
        // buttons the raw control-port contract exposes (m2_cport_bits.h) are
        // the eleven shipped pad buttons plus the vendor extended D/E/F, and
        // D/E/F belong with the controller buttons rather than the host
        // shortcuts.
        if (!action_row_sections_complete())
            return fail("Controls rows do not cover every Action exactly once");
        for (const auto& section : kActionRowSections) {
            if (!section.heading || !*section.heading || !section.column ||
                !*section.column || !section.table_id || !*section.table_id ||
                section.count == 0 || !section.rows)
                return fail("a Controls section is missing its heading or rows");
            for (size_t i = 0; i < section.count; ++i)
                if (*action_row_label(section.rows[i]) == '\0')
                    return fail("a Controls row has no label");
        }
        if (*action_row_label(Action::kActionCount) != '\0' ||
            std::string(action_row_label(Action::A)) != "A")
            return fail("Controls row labels changed");
        const Action exposed_buttons[] = {
            Action::DpadUp, Action::DpadDown, Action::DpadLeft, Action::DpadRight,
            Action::A, Action::B, Action::C, Action::X, Action::L, Action::R, Action::P,
            Action::D, Action::E, Action::F,
        };
        for (Action action : exposed_buttons) {
            bool in_controller_section = false;
            for (Action row : kControllerButtonRows)
                in_controller_section |= row == action;
            if (!in_controller_section)
                return fail("an exposed pad button is missing from the Controls controller section");
        }
        for (Action row : kControllerButtonRows)
            if (row == Action::Quit || row == Action::WheelLeft)
                return fail("a host-only or analog action was listed as a pad button");
    }
    {
        // Enhancements/Hacks tab sections: Bug Fixes, Restored Features, and
        // Hacks partition the checkbox options without dropping or duplicating
        // any flag, and preserve their option labels and descriptions.
        if (std::size(kEnhancementSections) != 3 ||
            std::string(kEnhancementSections[0].heading) != "Bug Fixes" ||
            std::string(kEnhancementSections[1].heading) != "Restored Features" ||
            std::string(kEnhancementSections[2].heading) != "Hacks")
            return fail("Enhancements/Hacks section headings changed");
        if (kEnhancementSections[0].count != 3 ||
            std::string(kEnhancementSections[0].options[0].flag) != "M2_SKIDPAD_ENGINE_FIX" ||
            std::string(kEnhancementSections[0].options[1].flag) != "M2_LAGUNA_PHANTOM_FIX" ||
            std::string(kEnhancementSections[0].options[2].flag) != "M2_HUD_TACH_FIX")
            return fail("Bug Fixes section options changed");
        if (kEnhancementSections[1].count != 3 ||
            std::string(kEnhancementSections[1].options[0].flag) != "M2_RESTORE_CREDITS" ||
            std::string(kEnhancementSections[1].options[1].flag) != "M2_RESTORE_DRAFTING" ||
            std::string(kEnhancementSections[1].options[2].flag) != "M2_RESTORE_BEST_TIMES")
            return fail("Restored Features section options changed");
        if (kEnhancementSections[2].count != 1 ||
            std::string(kEnhancementSections[2].options[0].flag) != "M2_UNLOCK_SECRET_TRACK")
            return fail("Hacks section options changed");
        std::vector<std::string> seen_enhancement_flags;
        for (const auto& section : kEnhancementSections) {
            for (size_t i = 0; i < section.count; ++i) {
                const auto& opt = section.options[i];
                if (!opt.flag || !is_launcher_flag(opt.flag) || !opt.label || !*opt.label ||
                    !opt.description || !*opt.description)
                    return fail("an Enhancements/Hacks option is missing its flag, label, or description");
                if (std::find(seen_enhancement_flags.begin(), seen_enhancement_flags.end(), opt.flag) !=
                    seen_enhancement_flags.end())
                    return fail("an Enhancements/Hacks option is duplicated across sections");
                seen_enhancement_flags.emplace_back(opt.flag);
            }
        }
        if (std::string(kCarVariantOptionLabel) != "Player car livery variant" ||
            !*kCarVariantOptionDescription)
            return fail("Player car livery variant label or description changed");
    }
    {
        // Curated merge: only the launcher's exposed flags arrive, each one
        // must be a real row in the generated catalog (a catalog rename must
        // not silently orphan a control), and names that lost their control
        // must neither be merged nor be treated as curated.
        Settings catalog;
        ensure_launcher_flags(catalog);
        if (catalog.flags.size() != size_t(kLauncherFlagCount))
            return fail("curated flag merge changed size");
        for (const char* name : kLauncherFlagNames) {
            if (find_flag(catalog, name) < 0 || !is_catalog_flag(name) ||
                !valid_flag(name) || !is_launcher_flag(name))
                return fail("a curated launcher flag is not a valid catalog row");
        }
        for (const auto& flag : catalog.flags) {
            const std::string name = flag.name.data();
            const bool default_enabled = name == "M2_IMPROVED_ANALOG_STEERING" || name == "M2_SKIDPAD_ENGINE_FIX";
            if (flag.enabled != default_enabled || (flag.enabled && std::string(flag.value.data()) != "1"))
                return fail("a curated flag has the wrong launcher default");
        }
        if (find_flag(catalog, "M2_CLT_TRACE") >= 0 ||
            is_launcher_flag("M2_CLT_TRACE") || is_launcher_flag(""))
            return fail("a non-curated flag was merged or treated as curated");
        // Opt-in booleans read the flag's own contract: only an exact saved
        // "1" is on, so a 0 or hand-typed value shows as unchecked; switching
        // the option off clears the value instead of exporting it.
        if (flag_is_on(catalog, "M2_RESTORE_DRAFTING") || flag_is_on(catalog, "M2_MISSING"))
            return fail("a fresh curated boolean read as on");
        set_flag_on(catalog, "M2_RESTORE_DRAFTING", true);
        const int drafting = find_flag(catalog, "M2_RESTORE_DRAFTING");
        if (!flag_is_on(catalog, "M2_RESTORE_DRAFTING") ||
            std::string(catalog.flags[size_t(drafting)].value.data()) != "1")
            return fail("turning drafting on did not set 1");
        set_flag_on(catalog, "M2_RESTORE_DRAFTING", false);
        if (flag_is_on(catalog, "M2_RESTORE_DRAFTING") ||
            catalog.flags[size_t(drafting)].value[0] != '\0')
            return fail("turning drafting off did not clear the value");
        copy(catalog.flags[size_t(drafting)].value, "0");
        catalog.flags[size_t(drafting)].enabled = true;
        if (flag_is_on(catalog, "M2_RESTORE_DRAFTING"))
            return fail("a saved 0 read as drafting on");
        copy(catalog.flags[size_t(drafting)].value, "2");
        if (flag_is_on(catalog, "M2_RESTORE_DRAFTING"))
            return fail("a saved non-1 value read as drafting on");
        set_flag_on(catalog, "M2_RESTORE_DRAFTING", false);
        // Autopilot is a debug-only environment flag, not a launcher option.
        if (is_launcher_flag("M2_PLAYER_AI") || find_flag(catalog, "M2_PLAYER_AI") >= 0)
            return fail("autopilot is still a launcher option");
        set_flag_on(catalog, "M2_MISSING", true);  // no row: no-op
        // The Graphics combos edit their rows through find_flag.
        if (find_flag(catalog, "M2_ANISOTROPY") < 0 ||
            find_flag(catalog, "M2_ANTIALIASING") < 0 ||
            find_flag(catalog, "M2_RENDER_SCALE") < 0 ||
            find_flag(catalog, "M2_TEXTURE_FILTER") < 0 ||
            find_flag(catalog, "M2_BITMAP_FILTER") < 0 ||
            find_flag(catalog, "M2_HIRES_EFFECTS") < 0 ||
            find_flag(catalog, "M2_DISABLE_CAR_LOD") < 0)
            return fail("generated catalog is missing a Graphics-tab flag");
        if (find_flag(catalog, "M2_PLAYER_CAR_VARIANT") < 0)
            return fail("generated catalog is missing the Enhancements-tab flag");
        // RGB555 dithering shares the exact-"1" boolean contract and starts off.
        if (find_flag(catalog, "M2_RGB555_DITHER") < 0)
            return fail("generated catalog is missing the dither flag");
        if (flag_is_on(catalog, "M2_RGB555_DITHER"))
            return fail("a fresh RGB555 dither option read as on");
        set_flag_on(catalog, "M2_RGB555_DITHER", true);
        if (!flag_is_on(catalog, "M2_RGB555_DITHER"))
            return fail("turning RGB555 dither on did not read as on");
        set_flag_on(catalog, "M2_RGB555_DITHER", false);
        if (flag_is_on(catalog, "M2_RGB555_DITHER"))
            return fail("turning RGB555 dither off did not read as off");
        // The car LOD override uses the same exact-1 opt-in contract.
        if (flag_is_on(catalog, "M2_DISABLE_CAR_LOD"))
            return fail("a fresh car LOD option read as on");
        set_flag_on(catalog, "M2_DISABLE_CAR_LOD", true);
        if (!flag_is_on(catalog, "M2_DISABLE_CAR_LOD"))
            return fail("turning car LOD on did not read as on");
        set_flag_on(catalog, "M2_DISABLE_CAR_LOD", false);
        if (flag_is_on(catalog, "M2_DISABLE_CAR_LOD"))
            return fail("turning car LOD off did not read as off");
        // Skidpad engine fix shares the opt-in boolean contract: exact "1"
        // is on, every other value reads as off and switching off clears.
        if (!flag_is_on(catalog, "M2_SKIDPAD_ENGINE_FIX"))
            return fail("a fresh skidpad fix read as off");
        set_flag_on(catalog, "M2_SKIDPAD_ENGINE_FIX", true);
        if (!flag_is_on(catalog, "M2_SKIDPAD_ENGINE_FIX"))
            return fail("turning skidpad fix on did not read as on");
        set_flag_on(catalog, "M2_SKIDPAD_ENGINE_FIX", false);
        if (flag_is_on(catalog, "M2_SKIDPAD_ENGINE_FIX"))
            return fail("turning skidpad fix off did not read as off");
        ensure_launcher_flags(catalog);
        if (flag_is_on(catalog, "M2_SKIDPAD_ENGINE_FIX"))
            return fail("saved skidpad fix opt-out was overridden");
        // Restore credits shares the opt-in boolean contract: exact "1"
        // is on, every other value reads as off and switching off clears.
        if (find_flag(catalog, "M2_RESTORE_CREDITS") < 0)
            return fail("generated catalog is missing the credits flag");
        if (flag_is_on(catalog, "M2_RESTORE_CREDITS"))
            return fail("a fresh restore credits option read as on");
        set_flag_on(catalog, "M2_RESTORE_CREDITS", true);
        if (!flag_is_on(catalog, "M2_RESTORE_CREDITS"))
            return fail("turning restore credits on did not read as on");
        set_flag_on(catalog, "M2_RESTORE_CREDITS", false);
        if (flag_is_on(catalog, "M2_RESTORE_CREDITS"))
            return fail("turning restore credits off did not read as off");
        if (find_flag(catalog, "M2_RESTORE_BEST_TIMES") < 0)
            return fail("generated catalog is missing the best-times flag");
        if (flag_is_on(catalog, "M2_RESTORE_BEST_TIMES"))
            return fail("a fresh restore best-times option read as on");
        set_flag_on(catalog, "M2_RESTORE_BEST_TIMES", true);
        if (!flag_is_on(catalog, "M2_RESTORE_BEST_TIMES"))
            return fail("turning restore best times on did not read as on");
        set_flag_on(catalog, "M2_RESTORE_BEST_TIMES", false);
        if (flag_is_on(catalog, "M2_RESTORE_BEST_TIMES"))
            return fail("turning restore best times off did not read as off");
        if (find_flag(catalog, "M2_RESTORE_DRAFTING") < 0)
            return fail("generated catalog is missing the drafting flag");
        if (flag_is_on(catalog, "M2_RESTORE_DRAFTING"))
            return fail("a fresh restore drafting option read as on");
        set_flag_on(catalog, "M2_RESTORE_DRAFTING", true);
        if (!flag_is_on(catalog, "M2_RESTORE_DRAFTING"))
            return fail("turning restore drafting on did not read as on");
        set_flag_on(catalog, "M2_RESTORE_DRAFTING", false);
        if (flag_is_on(catalog, "M2_RESTORE_DRAFTING"))
            return fail("turning restore drafting off did not read as off");
        if (find_flag(catalog, "M2_UNLOCK_SECRET_TRACK") < 0)
            return fail("generated catalog is missing the secret-track unlock flag");
        if (flag_is_on(catalog, "M2_UNLOCK_SECRET_TRACK"))
            return fail("a fresh unlock secret track option read as on");
        set_flag_on(catalog, "M2_UNLOCK_SECRET_TRACK", true);
        if (!flag_is_on(catalog, "M2_UNLOCK_SECRET_TRACK"))
            return fail("turning unlock secret track on did not read as on");
        set_flag_on(catalog, "M2_UNLOCK_SECRET_TRACK", false);
        if (flag_is_on(catalog, "M2_UNLOCK_SECRET_TRACK"))
            return fail("turning unlock secret track off did not read as off");
        // Default-on: a fresh row is unset (the game applies the fix), turning
        // it off exports exactly 0, and turning it back on unsets it again.
        const int laguna = find_flag(catalog, "M2_LAGUNA_PHANTOM_FIX");
        if (laguna < 0)
            return fail("generated catalog is missing the Laguna phantom fix flag");
        auto& laguna_row = catalog.flags[size_t(laguna)];
        if (!option_is_on(catalog, "M2_LAGUNA_PHANTOM_FIX") ||
            should_export_flag(laguna_row.enabled, laguna_row.value.data()))
            return fail("a fresh Laguna phantom fix option was off or exported");
        set_option_on(catalog, "M2_LAGUNA_PHANTOM_FIX", false);
        if (option_is_on(catalog, "M2_LAGUNA_PHANTOM_FIX") ||
            !should_export_flag(laguna_row.enabled, laguna_row.value.data()) ||
            std::string(laguna_row.value.data()) != "0")
            return fail("turning the Laguna phantom fix off did not export 0");
        set_option_on(catalog, "M2_LAGUNA_PHANTOM_FIX", true);
        if (!option_is_on(catalog, "M2_LAGUNA_PHANTOM_FIX") ||
            should_export_flag(laguna_row.enabled, laguna_row.value.data()))
            return fail("turning the Laguna phantom fix on did not unset it");
        laguna_row.enabled = true;
        copy(laguna_row.value, "1");
        if (!option_is_on(catalog, "M2_LAGUNA_PHANTOM_FIX"))
            return fail("a saved Laguna phantom fix value of 1 read as off");
        // The exterior tachometer range fix is default-on in the same way.
        const int tach = find_flag(catalog, "M2_HUD_TACH_FIX");
        if (tach < 0)
            return fail("generated catalog is missing the exterior tach fix flag");
        auto& tach_row = catalog.flags[size_t(tach)];
        if (!option_is_on(catalog, "M2_HUD_TACH_FIX") ||
            should_export_flag(tach_row.enabled, tach_row.value.data()))
            return fail("a fresh exterior tach fix option was off or exported");
        set_option_on(catalog, "M2_HUD_TACH_FIX", false);
        if (option_is_on(catalog, "M2_HUD_TACH_FIX") ||
            !should_export_flag(tach_row.enabled, tach_row.value.data()) ||
            std::string(tach_row.value.data()) != "0")
            return fail("turning the exterior tach fix off did not export 0");
        set_option_on(catalog, "M2_HUD_TACH_FIX", true);
        if (!option_is_on(catalog, "M2_HUD_TACH_FIX") ||
            should_export_flag(tach_row.enabled, tach_row.value.data()))
            return fail("turning the exterior tach fix on did not unset it");
        // Overscan crop is default-on in the same way: fresh rows are unset,
        // off exports exactly 0, on unsets it again.
        const int crop = find_flag(catalog, "M2_CROP_OVERSCAN");
        if (crop < 0)
            return fail("generated catalog is missing the overscan crop flag");
        auto& crop_row = catalog.flags[size_t(crop)];
        if (!option_is_on(catalog, "M2_CROP_OVERSCAN") ||
            should_export_flag(crop_row.enabled, crop_row.value.data()))
            return fail("a fresh overscan crop option was off or exported");
        set_option_on(catalog, "M2_CROP_OVERSCAN", false);
        if (option_is_on(catalog, "M2_CROP_OVERSCAN") ||
            !should_export_flag(crop_row.enabled, crop_row.value.data()) ||
            std::string(crop_row.value.data()) != "0")
            return fail("turning the overscan crop off did not export 0");
        set_option_on(catalog, "M2_CROP_OVERSCAN", true);
        if (!option_is_on(catalog, "M2_CROP_OVERSCAN") ||
            should_export_flag(crop_row.enabled, crop_row.value.data()))
            return fail("turning the overscan crop on did not unset it");
        // Opt-in options keep their exact-1 contract through the same helpers.
        set_option_on(catalog, "M2_UNLOCK_SECRET_TRACK", true);
        if (!flag_is_on(catalog, "M2_UNLOCK_SECRET_TRACK"))
            return fail("set_option_on changed an opt-in option's contract");
        set_option_on(catalog, "M2_UNLOCK_SECRET_TRACK", false);
        if (option_is_on(catalog, "M2_UNLOCK_SECRET_TRACK"))
            return fail("an opt-in option read as on after set_option_on(false)");
    }
    {
        // Version 1 is the only launcher.cfg format.
        std::ofstream file(path);
        file << "m2-launcher 1\n\"/game/m2host\"\n\"/game/data\"\n"
                "960 720 1\n0\n0 0\n0\n1\n"
                "1 \"M2_CLT_BRIDGE_TRACE\" \"1\"\n";
    }
    Settings settings;
    std::string message;
    if (!load_settings(path, settings, message) ||
        std::string(settings.executable.data()) != "/game/m2host" ||
        settings.use_host_defaults || !settings.fast_forward ||
        settings.flags.size() != 1 || settings.controls_mode != M2_CONTROLS_PATCHED ||
        std::string(settings.flags[0].value.data()) != "1")
        return fail("version 1 settings lost the saved launch configuration");
    for (const char* other : {"m2-launcher 2\n", "m2-launcher 0\n", "m2-launcher-x 1\n"}) {
        std::ofstream file(path);
        file << other << "\"/game/m2host\"\n\"/game/data\"\n960 720 1\n0\n0 0\n0\n1\n";
        file.close();
        Settings rejected;
        if (load_settings(path, rejected, message))
            return fail("launcher settings with another header or version were accepted");
    }
    Settings fresh;
    if (fresh.controls_mode != M2_CONTROLS_CUSTOM)
        return fail("Custom is not the fresh launcher default");
    if (controls_tab(M2_CONTROLS_CUSTOM) != 0 ||
        kControlsTabModes[size_t(controls_tab(M2_CONTROLS_ORIGINAL))] != M2_CONTROLS_ORIGINAL ||
        kControlsTabModes[size_t(controls_tab(M2_CONTROLS_PATCHED))] != M2_CONTROLS_PATCHED)
        return fail("Controls tabs do not map back to their modes");
    copy(fresh.executable, "/game/m2host"); copy(fresh.data, "/game/data");
    for (bool host_defaults : {false, true}) {
        fresh.use_host_defaults = host_defaults;
        const auto args = launch_arguments(fresh, "/profiles/controls.cfg");
        if (args[3] != "--controls-mode" || args[4] != "custom")
            return fail("the default launcher mode did not reach the host as Custom");
    }
    // Argument-shape checks with explicit Original mode.
    settings.controls_mode = M2_CONTROLS_ORIGINAL;
    // Custom launches always use the live controls profile, and with no
    // replay concept left fast forward is honored whenever it is selected.
    const auto custom_args = launch_arguments(settings, "/profiles/controls.cfg");
    if (custom_args.size() != 12 || custom_args[1] != "--root" ||
        custom_args[3] != "--width" || custom_args[5] != "--height" ||
        custom_args[7] != "--controls-profile" ||
        custom_args[8] != "/profiles/controls.cfg" ||
        custom_args[9] != "--fast-forward" ||
        custom_args[10] != "--controller-type" || custom_args[11] != "pad")
        return fail("custom launch arguments changed");
    Settings live_settings = settings;
    live_settings.fast_forward = false;
    const auto live_args = launch_arguments(live_settings, "/profiles/controls.cfg");
    if (live_args.size() != 11 || live_args[7] != "--controls-profile" ||
        live_args[8] != "/profiles/controls.cfg" ||
        live_args[9] != "--controller-type" || live_args[10] != "pad")
        return fail("custom live controls profile was lost");
    if (!save_settings(path, settings, message))
        return fail("settings did not save");
    // Closed before the next save: Windows will not rename over an open file.
    std::string text;
    {
        std::ifstream saved(path);
        text.assign(std::istreambuf_iterator<char>(saved),
                    std::istreambuf_iterator<char>());
    }
    if (text.rfind("m2-launcher 1\n", 0) != 0)
        return fail("settings were not saved as version 1");
    Settings reloaded;
    if (!load_settings(path, reloaded, message) ||
        std::string(reloaded.executable.data()) != "/game/m2host" ||
        reloaded.flags.size() != 1 ||
        std::string(reloaded.flags[0].name.data()) != "M2_CLT_BRIDGE_TRACE")
        return fail("settings did not round trip");
    // A hand-added row survives in the file but is not a launcher option.
    if (is_launcher_flag(reloaded.flags[0].name.data()))
        return fail("a hand-added flag became a launcher option");
    reloaded.use_host_defaults = true;
    const auto default_args = launch_arguments(reloaded, "/profiles/controls.cfg");
    // launch_arguments makes paths absolute, which adds a drive on Windows,
    // so expected paths are compared as absolute paths.
    if (default_args.size() != 5 ||
        fs::path(default_args[0]) != fs::absolute("/game/m2host") ||
        default_args[1] != "--root" || default_args[2] != launch_data_root(reloaded, false) ||
        default_args[3] != "--controller-type" || default_args[4] != "pad")
        return fail("host-default launch must pass the executable, data root, and controller type");
    if (!save_settings(path, reloaded, message))
        return fail("host-default settings did not save");
    Settings default_reloaded;
    if (!load_settings(path, default_reloaded, message) ||
        !default_reloaded.use_host_defaults ||
        launch_arguments(default_reloaded, "/profiles/controls.cfg").size() != 5)
        return fail("host-default launch choice did not round trip");
    {
        // Host defaults and an opt-in option row round trip together.
        Settings drafting = settings;
        drafting.use_host_defaults = true;
        ensure_launcher_flags(drafting);
        set_flag_on(drafting, "M2_RESTORE_DRAFTING", true);
        if (!save_settings(path, drafting, message)) return fail("drafting settings did not save");
        Settings loaded;
        if (!load_settings(path, loaded, message) || !loaded.use_host_defaults ||
            !flag_is_on(loaded, "M2_RESTORE_DRAFTING"))
            return fail("drafting settings did not round trip");
    }
    // The launcher must pair the second image with its own disc assets.
    Settings shooter = settings;
    copy(shooter.data, "/disc/extracted/mycd/driving/data/");
    shooter.use_host_defaults = false;
    const auto shooter_args = launch_arguments(shooter, "/profiles/controls.cfg", true);
    const auto driving_args = launch_arguments(shooter, "/profiles/controls.cfg", false);
    if (shooter_args[1] != "--root" ||
        fs::path(shooter_args[2]) != fs::absolute("/disc/extracted/mycd/shooter2d") ||
        fs::path(driving_args[2]) != fs::absolute("/disc/extracted/mycd/driving/data") ||
        shooter_args[7] != "--controls-profile" || shooter_args[8] != "/profiles/controls.cfg")
        return fail("application image and asset directory disagree");
    shooter.use_host_defaults = true;
    if (launch_arguments(shooter, "/profiles/controls.cfg", true).size() != 5 ||
        fs::path(launch_arguments(shooter, "/profiles/controls.cfg", true)[2]) !=
            fs::absolute("/disc/extracted/mycd/shooter2d") ||
        launch_arguments(shooter, "/profiles/controls.cfg", true)[3] != "--controller-type")
        return fail("host-default shooter launch must pass the managed app-specific data root");
    // Each button explicitly chooses its image, overriding an inherited choice.
    SDL_Environment* env = SDL_CreateEnvironment(false);
    if (!env) return fail("cannot create child environment");
    SDL_SetEnvironmentVariable(env, "M2_BOOT_APP", "shooter2d", true);
    if (!apply_boot_option(false, env) ||
        std::string(SDL_GetEnvironmentVariable(env, "M2_BOOT_APP")) != "game")
        return fail("Launch game did not select driving");
    if (!apply_boot_option(true, env) ||
        std::string(SDL_GetEnvironmentVariable(env, "M2_BOOT_APP")) != "shooter2d")
        return fail("Launch shooter2d did not select shooter");
    if (!apply_boot_option(false, env) ||
        std::string(SDL_GetEnvironmentVariable(env, "M2_BOOT_APP")) != "game")
        return fail("shooter launch leaked into the next game launch");
    SDL_SetEnvironmentVariable(env, "M2_MICROCARD_ROOT", "/stale/card", true);
    if (!apply_save_option("/managed saves/card", env) ||
        std::string(SDL_GetEnvironmentVariable(env, "M2_MICROCARD_ROOT")) != "/managed saves/card")
        return fail("game save location differs from launcher management location");
    if (!apply_boot_option(true, env) ||
        std::string(SDL_GetEnvironmentVariable(env, "M2_MICROCARD_ROOT")) != "/managed saves/card")
        return fail("shooter launch changed the shared save location");
    if (apply_save_option({}, env)) return fail("empty save root was accepted");
    SDL_DestroyEnvironment(env);
    Settings roundtrip;
    {
        Settings base;
        copy(base.executable, "/game/m2host"); copy(base.data, "/game/data");
        base.width = 800; base.height = 600; base.use_host_defaults = true;
        ensure_launcher_flags(base);
        set_flag_on(base, "M2_RESTORE_DRAFTING", true);
        if (!save_settings(path, base, message) || !load_settings(path, roundtrip, message) ||
            !roundtrip.use_host_defaults || !flag_is_on(roundtrip, "M2_RESTORE_DRAFTING"))
            return fail("settings did not round trip");
    }
    {
        // Controller type is a host setting independent of bindings.
        Settings joystick = roundtrip;
        joystick.controller_type = ControllerType::Joystick;
        if (!save_settings(path, joystick, message))
            return fail("controller type did not save");
        Settings loaded_joystick;
        if (!load_settings(path, loaded_joystick, message) ||
            loaded_joystick.controller_type != ControllerType::Joystick)
            return fail("controller type did not round trip");
        for (auto mode : {M2_CONTROLS_ORIGINAL, M2_CONTROLS_PATCHED, M2_CONTROLS_CUSTOM}) {
            Settings controls = roundtrip;
            controls.controls_mode = mode;
            controls.controller_type = ControllerType::Joystick;
            if (!save_settings(path, controls, message)) return fail("control mode did not save");
            Settings loaded;
            if (!load_settings(path, loaded, message) || loaded.controls_mode != mode)
                return fail("control mode did not round trip");
            const auto args = launch_arguments(loaded, "/profiles/controls.cfg");
            if (mode == M2_CONTROLS_ORIGINAL) {
                if (std::find(args.begin(), args.end(), "--controls-mode") != args.end() || args.back() != "joystick")
                    return fail("an explicitly saved Original choice was overridden");
            } else if (args[3] != "--controls-mode" || args[4] != (mode == M2_CONTROLS_PATCHED ? "patched" : "custom") ||
                args.back() != "pad") return fail("enhanced controls launch uses wrong mode/device");
            const auto shooter = launch_arguments(loaded, "/profiles/controls.cfg", true);
            const bool shooter_mode =
                std::find(shooter.begin(), shooter.end(), "--controls-mode") != shooter.end();
            if (shooter_mode != (mode == M2_CONTROLS_CUSTOM))
                return fail("only Custom controls apply to shooter2d");
            if (shooter.back() != (mode == M2_CONTROLS_CUSTOM ? "pad" : "joystick"))
                return fail("shooter2d launch uses the wrong controller type");
        }
        if (!binding_context_overlaps(Action::ShooterFire, Action::ShooterBeam) ||
            !binding_context_overlaps(Action::ShooterStart, Action::DrivePause) ||
            !binding_context_overlaps(Action::ShooterFire, Action::DriveScreenshot) ||
            !binding_context_overlaps(Action::DriveAnalogLeft, Action::ShooterBeam) ||
            !binding_context_overlaps(Action::ShooterFire, Action::Quit) ||
            binding_context_overlaps(Action::ShooterFire, Action::DriveThrottle) ||
            binding_context_overlaps(Action::ShooterStart, Action::DriveAids) ||
            binding_context_overlaps(Action::MenuConfirm, Action::ShooterFire) ||
            binding_context_overlaps(Action::ShooterFire, Action::A))
            return fail("shooter2d binding conflicts cross unrelated actions");
        if (binding_context_overlaps(Action::DriveLeft, Action::MenuLeft) ||
            binding_context_overlaps(Action::A, Action::DriveThrottle) ||
            !binding_context_overlaps(Action::DriveLeft, Action::DriveRight) ||
            !binding_context_overlaps(Action::DriveLeft, Action::DriveAnalogLeft))
            return fail("binding conflicts cross unrelated control contexts");
        const Action directions[] = {Action::AnalogUp, Action::AnalogDown, Action::DriveAnalogLeft,
            Action::DriveAnalogRight, Action::DigitalUp, Action::DigitalDown, Action::DriveLeft, Action::DriveRight};
        for (size_t i = 0; i < std::size(directions); ++i)
            if (kDrivingRows[i] != directions[i]) return fail("Custom direction order changed");
        const std::pair<Action, const char*> shooter_effects[] = {
            {Action::DpadUp, "Move"}, {Action::DpadDown, "Move"}, {Action::DpadLeft, "Move"},
            {Action::DpadRight, "Move"}, {Action::A, "Fire"}, {Action::B, "Beam"}, {Action::C, "Start"},
            {Action::P, "Pause"}, {Action::E, "Pause"}, {Action::R, "Screenshot"},
            {Action::L, "Screenshot timing"}, {Action::X, "N/A"}, {Action::D, "N/A"}, {Action::F, "N/A"},
            {Action::StickLeft, "N/A"}, {Action::WheelRight, "N/A"}, {Action::Quit, "Close the game"},
        };
        for (const auto& [action, effect] : shooter_effects)
            if (std::string(shooter_action_effect(action)) != effect)
                return fail("shooter2d control effects are wrong");
        if (std::string(action_effect(Action::A, M2_CONTROLS_ORIGINAL, ControllerType::Pad)) != "Confirm in menus / throttle" ||
            std::string(action_effect(Action::B, M2_CONTROLS_ORIGINAL, ControllerType::Pad)) != "Back in menus / brake" ||
            std::string(action_effect(Action::F, M2_CONTROLS_ORIGINAL, ControllerType::Pad)) != "Change view")
            return fail("digital action effects are wrong");
        if (std::string(action_effect(Action::L, M2_CONTROLS_ORIGINAL, ControllerType::Pad)) != "Shift down" ||
            std::string(action_effect(Action::R, M2_CONTROLS_ORIGINAL, ControllerType::Pad)) != "Shift up")
            return fail("digital shoulder shift directions are reversed");
        if (std::string(action_effect(Action::DriveShiftUp, M2_CONTROLS_CUSTOM, ControllerType::Pad)) != "Shift up" ||
            std::string(action_effect(Action::DriveShiftDown, M2_CONTROLS_CUSTOM, ControllerType::Pad)) != "Shift down")
            return fail("Custom shift wording no longer matches the launcher rows");
        for (const char* label : kActionLabels)
            if (std::string(label) == "Clutch")
                return fail("manual clutch action remains in the launcher");
        for (auto mode : {M2_CONTROLS_ORIGINAL, M2_CONTROLS_PATCHED, M2_CONTROLS_CUSTOM}) {
            for (int layout = 0; layout < 3; ++layout) {
                const Action screenshot = mode == M2_CONTROLS_CUSTOM ? Action::DriveScreenshot
                    : mode == M2_CONTROLS_PATCHED && layout == 0 ? Action::B
                    : mode == M2_CONTROLS_PATCHED && layout == 2 ? Action::R : Action::D;
                if (!is_screenshot_action(screenshot, mode, ControllerType::Pad, layout) ||
                    is_screenshot_action(Action::DriveView, mode, ControllerType::Pad, layout))
                    return fail("screenshot tooltip assigned to the wrong controller action");
            }
        }
        if (!is_screenshot_action(Action::R, M2_CONTROLS_ORIGINAL, ControllerType::Joystick) ||
            is_screenshot_action(Action::D, M2_CONTROLS_ORIGINAL, ControllerType::Joystick) ||
            !binding_context_overlaps(Action::DriveScreenshot, Action::DriveView) ||
            binding_context_overlaps(Action::DriveScreenshot, Action::MenuBack))
            return fail("screenshot binding context or analog tooltip incorrect");
        for (auto mode : {M2_CONTROLS_ORIGINAL, M2_CONTROLS_PATCHED, M2_CONTROLS_CUSTOM}) {
            for (int layout = 0; layout < 3; ++layout) {
                const Action hud = mode == M2_CONTROLS_CUSTOM ? Action::DriveAids
                    : mode == M2_CONTROLS_PATCHED && layout == 0 ? Action::A
                    : mode == M2_CONTROLS_PATCHED && layout == 2 ? Action::B : Action::C;
                if (!is_hud_cycle_action(hud, mode, ControllerType::Pad, layout) ||
                    is_hud_cycle_action(Action::DriveView, mode, ControllerType::Pad, layout))
                    return fail("HUD tooltip assigned to the wrong controller action");
            }
        }
        if (!is_hud_cycle_action(Action::B, M2_CONTROLS_ORIGINAL, ControllerType::Joystick) ||
            is_hud_cycle_action(Action::C, M2_CONTROLS_ORIGINAL, ControllerType::Joystick))
            return fail("analog HUD tooltip confused with shift-down");
    }
    {
        // Fullscreen: settings default to windowed; the choice round-trips and
        // reaches the host as exactly one fullscreen argument.
        if (roundtrip.fullscreen || roundtrip.borderless)
            return fail("default settings did not load windowed");
        Settings fs_settings = roundtrip;
        copy(fs_settings.executable, "/game/m2host");
        copy(fs_settings.data, "/game/data/driving/data");
        fs_settings.use_host_defaults = false;  // host defaults omit display arguments
        const auto has = [](const std::vector<std::string>& args, const char* arg) {
            return std::find(args.begin(), args.end(), arg) != args.end();
        };
        auto args = launch_arguments(fs_settings, "/profiles/controls.cfg");
        if (has(args, "--fullscreen") || has(args, "--borderless-fullscreen"))
            return fail("windowed launch requested fullscreen");
        fs_settings.fullscreen = true;
        args = launch_arguments(fs_settings, "/profiles/controls.cfg");
        if (!has(args, "--fullscreen") || has(args, "--borderless-fullscreen"))
            return fail("fullscreen launch argument is wrong");
        fs_settings.borderless = true;
        args = launch_arguments(fs_settings, "/profiles/controls.cfg");
        if (has(args, "--fullscreen") || !has(args, "--borderless-fullscreen"))
            return fail("borderless fullscreen launch argument is wrong");
        fs_settings.fullscreen = false;  // borderless alone is not fullscreen
        args = launch_arguments(fs_settings, "/profiles/controls.cfg");
        if (has(args, "--fullscreen") || has(args, "--borderless-fullscreen"))
            return fail("borderless without fullscreen launched fullscreen");
        fs_settings.fullscreen = true;
        if (!save_settings(path, fs_settings, message)) return fail("fullscreen settings did not save");
        Settings loaded_fs;
        if (!load_settings(path, loaded_fs, message) || !loaded_fs.fullscreen || !loaded_fs.borderless)
            return fail("fullscreen settings did not round trip");
        std::ofstream bad(path);
        bad << "m2-launcher 1\n\"/game/m2host\"\n\"/game/data\"\n800 600 0\n0\n2 0\n0\n1\n";
        bad.close();
        Settings rejected;
        if (load_settings(path, rejected, message))
            return fail("out-of-range fullscreen value was accepted");
    }
    {
        Settings steering;
        ensure_launcher_flags(steering);
        const char* flag = "M2_IMPROVED_ANALOG_STEERING";
        if (!flag_is_on(steering, flag)) return fail("improved steering defaulted off");
        set_flag_on(steering, flag, true);
        for (auto mode : {M2_CONTROLS_ORIGINAL, M2_CONTROLS_PATCHED, M2_CONTROLS_CUSTOM}) {
            steering.controls_mode = mode;
            if (!save_settings(path, steering, message)) return fail("steering settings did not save");
            Settings loaded;
            if (!load_settings(path, loaded, message) || !flag_is_on(loaded, flag) || loaded.controls_mode != mode)
                return fail("improved steering did not round trip with each profile");
        }
        set_flag_on(steering, flag, false);
        if (flag_is_on(steering, flag) || steering.flags[size_t(find_flag(steering, flag))].enabled)
            return fail("steering opt-out remained enabled");
        ensure_launcher_flags(steering);
        if (flag_is_on(steering, flag)) return fail("saved steering opt-out was overridden");
    }
    fs::remove(path);
    return 0;
}
