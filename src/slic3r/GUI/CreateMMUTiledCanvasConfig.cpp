#include "CreateMMUTiledCanvas.hpp"

#include "I18N.hpp"
#ifdef WIN32
#include "libslic3r/AppConfig.hpp"
#endif
#include "libslic3r/Config.hpp"
#include "libslic3r/Utils.hpp"
#include "format.hpp"
#include "GUI.hpp"
#include "Tab.hpp"
#include <wx/notebook.h>

#include <iostream>

#include <wx/wx.h>
#include <wx/brush.h>
#include <wx/scrolwin.h>
#include <wx/file.h>
#include <wx/mimetype.h>
#include <wx/odcombo.h>
#include <wx/rawbmp.h>
#include <wx/textctrl.h>
#include <wx/wrapsizer.h>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>


#include "CreateMMUTiledCanvasShared.hpp"

namespace Slic3r {
namespace GUI {

using namespace CreateMMUTiledCanvasDetail;

CreateMMUTiledCanvas::~CreateMMUTiledCanvas() {

}

void CreateMMUTiledCanvas::save_config()
{
    std::string config_str;
    for (const std::string& key : m_config.keys())
        config_str += key + " = " + m_config.opt_serialize(key) + "\n";

    boost::filesystem::path path_dir = Slic3r::data_dir();
    path_dir = path_dir / "generator";
    if (!boost::filesystem::exists(path_dir))
        boost::filesystem::create_directories(path_dir);
    boost::filesystem::path path_ini = path_dir / "config.ini";
    boost::filesystem::path path_temp = path_dir / "config.ini.temp";

    boost::nowide::ofstream c;
    c.open(path_temp.string(), std::ios::out | std::ios::trunc);
    c << config_str;
#ifdef WIN32
    // WIN32 specific: The final "rename_file()" call is not safe in case of an application crash, there is no atomic "rename file" API
    // provided by Windows (sic!). Therefore we save a MD5 checksum to be able to verify file corruption. In addition,
    // we save the config file into a backup first before moving it to the final destination.
    c << AppConfig::appconfig_md5_hash_line(config_str);
#endif
    c.close();
#ifdef WIN32
    // Make a backup of the configuration file before copying it to the final destination.
    std::string error_message;
    boost::filesystem::path path_bak = path_dir / "config.ini.bak";
    // Copy configuration file with PID suffix into the configuration file with "bak" suffix.
    if (copy_file(path_temp.string(), path_bak.string(), error_message, false) != SUCCESS)
        BOOST_LOG_TRIVIAL(error) << "Copying from " << path_temp.string() << " to " << path_bak.string() << " failed. Failed to create a backup configuration.";
#endif
    // Rename the config atomically.
    // On Windows, the rename is likely NOT atomic, thus it may fail if PrusaSlicer crashes on another thread in the meanwhile.
    // To cope with that, we already made a backup of the config on Windows.
    if (const std::error_code error = rename_file(path_temp.string(), path_ini.string())) {
        BOOST_LOG_TRIVIAL(error) << "Renaming " << path_temp.string() << " to " << path_ini.string()
                                 << " failed: " << error.message();
        return;
    }
    m_dirty = false;
}

#ifdef WIN32
namespace {
bool restore_mmu_canvas_config(
    const std::string& path,
    const std::string& initial_error,
    boost::property_tree::ptree& tree)
{
    const std::string backup_path = (boost::format("%1%.bak") % path).str();
    if (!boost::filesystem::exists(backup_path)) {
        BOOST_LOG_TRIVIAL(info) << format(
            R"(Failed to parse configuration file "%1%": %2%)", path, initial_error);
        return false;
    }

    boost::nowide::ifstream backup_ifs(backup_path);
    const AppConfig::ConfigFileInfo config_file_info =
        AppConfig::check_config_file_and_verify_checksum(backup_ifs);
    if (!config_file_info.correct_checksum || config_file_info.contains_null) {
        BOOST_LOG_TRIVIAL(error) << "Both \"" << path << "\" and \"" << backup_path
            << "\" are corrupted. It isn't possible to restore configuration from the backup.";
        backup_ifs.close();
        boost::filesystem::remove(backup_path);
        return false;
    }

    std::string error_message;
    if (copy_file(backup_path, path, error_message, false) != SUCCESS) {
        BOOST_LOG_TRIVIAL(error) << "Configuration file \"" << path
            << "\" is corrupted. Failed to restore from backup \"" << backup_path
            << "\": " << error_message;
        backup_ifs.close();
        boost::filesystem::remove(backup_path);
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Configuration file \"" << path
        << "\" was corrupted. It has been successfully restored from the backup \""
        << backup_path << "\".";
    try {
        boost::nowide::ifstream restored_ifs(path);
        boost::property_tree::read_ini(restored_ifs, tree);
        return true;
    } catch (const boost::property_tree::ptree_error& ex) {
        BOOST_LOG_TRIVIAL(info) << format(
            R"(Failed to parse configuration file "%1%" after it has been restored from backup: %2%)",
            path, ex.what());
        return false;
    }
}
} // namespace
#endif

void CreateMMUTiledCanvas::load_config()
{
    if (m_config.config_def.options.empty()) {
        //create defs
        ConfigOptionDef def;

        def = ConfigOptionDef{"size", coPoint};
        def.label = L("Pixels count");
        def.tooltip = L("Number of pixels in x and y axis to include in the tile.");
        def.sidetext = L("px");
        def.min = 0;
        def.set_default_value(std::make_unique<ConfigOptionPoint>(ConfigOptionPoint{ Vec2d{ kDefaultTilePixels, kDefaultTilePixels } }));
        m_config.config_def.options["size"] = def;
        m_config.set_key_value("size", def.default_value.get()->clone());

        def = ConfigOptionDef{"size_px", coPoint};
        def.label = L("Pixel Size");
        def.tooltip = L("Size in mm on x and y axis for a pixel");
        def.sidetext = L("mm");
        def.min = 0;
        def.set_default_value(std::make_unique<ConfigOptionPoint>(ConfigOptionPoint{ Vec2d{ kDefaultPointCoordinate, kDefaultPointCoordinate } }));
        m_config.config_def.options["size_px"] = def;
        m_config.set_key_value("size_px", def.default_value.get()->clone());

        def = ConfigOptionDef{"height", coFloat};
        def.label = L("height");
        def.tooltip = L("Height of the full object.");
        def.set_default_value(std::make_unique<ConfigOptionFloat>(ConfigOptionFloat{ 1 }));
        def.sidetext = L("mm");
        def.min = 0;
        def.width = kConfigFieldWidthChars;
        m_config.config_def.options["height"] = def;
        m_config.set_key_value("height", def.default_value.get()->clone());

        def = ConfigOptionDef{"offset", coPoint};
        def.label = L("Offset");
        def.tooltip = L("First pixel position (top left corner) in the image.");
        def.sidetext = L("mm");
        def.min = 0;
        def.set_default_value(std::make_unique<ConfigOptionPoint>(ConfigOptionPoint{ Vec2d{ kDefaultOffsetXPx, kDefaultOffsetYPx } }));
        m_config.config_def.options["offset"] = def;
        m_config.set_key_value("offset", def.default_value.get()->clone());


        def = ConfigOptionDef{kSeparationXyKey, coFloat};
        def.label = L("XY");
        def.tooltip = L("Number of mm between (pixel) tiles.");
        //def.sidetext = L("mm");
        def.min = 0;
        def.width = kConfigFieldWidthChars;
        def.set_default_value(std::make_unique<ConfigOptionFloat>(ConfigOptionFloat{ 1 }));
        m_config.config_def.options[kSeparationXyKey] = def;
        m_config.set_key_value(kSeparationXyKey, def.default_value.get()->clone());

        def = ConfigOptionDef{kSeparationZKey, coFloat};
        def.label = L("Z");
        def.tooltip = L("Height of the separation. If higher than the height, then tiles won't be joined."
            "\nA zero value isn't supported yet. Please use at least one layer of separation.");
        def.sidetext = L("mm");
        def.min = kMinSeparationZMm;
        def.width = kConfigFieldWidthChars;
        def.set_default_value(std::make_unique<ConfigOptionFloat>(ConfigOptionFloat{ kDefaultSeparationZMm }));
        m_config.config_def.options[kSeparationZKey] = def;
        m_config.set_key_value(kSeparationZKey, def.default_value.get()->clone());

        def = ConfigOptionDef{"bezel", coFloat};
        def.label = L("Bezel");
        def.tooltip = L("Bevel for the bottom of the 'pillars'. Should be less than half of Pixel Size");
        def.sidetext = L("mm");
        def.min = 0.0;
        def.width = kConfigFieldWidthChars;
        def.set_default_value(std::make_unique<ConfigOptionFloat>(ConfigOptionFloat{ 0.0 }));
        m_config.config_def.options["bezel"] = def;
        m_config.set_key_value("bezel", def.default_value.get()->clone());

        def = ConfigOptionDef{"border", coFloat};
        def.label = L("Border");
        def.tooltip = L("Border width over the mosaic plate.");
        def.sidetext = L("mm");
        def.min = 0.0;
        def.width = kConfigFieldWidthChars;
        def.set_default_value(std::make_unique<ConfigOptionFloat>(ConfigOptionFloat{ 0.0 }));
        m_config.config_def.options["border"] = def;
        m_config.set_key_value("border", def.default_value.get()->clone());
        

        //not implemented yet
        //def = ConfigOptionDef();
        //def.label = L("Z Bump");
        //def.type = coFloat;
        //def.tooltip = L("If you want to have a 3D shaped tile, set this setting. It's the height diff at the center of the tile.");
        //def.sidetext = L("mm");
        //def.set_default_value(std::make_unique<ConfigOptionFloat>(ConfigOptionFloat{ 0 }));
        //m_config.config_def.options["bump"] = def;
        //m_config.set_key_value("bump", def.default_value.get()->clone());

        def = ConfigOptionDef{kNearColorKey, coBool};
        def.label = L("Use nearest color");
        def.tooltip = L("If there is not enough extruders configured, use the one with the nearest color. It will use the nearest spool color if spool_colors is set.");
        def.set_default_value(std::make_unique<ConfigOptionBool>(ConfigOptionBool{ true }));
        m_config.config_def.options[kNearColorKey] = def;
        m_config.set_key_value(kNearColorKey, def.default_value.get()->clone());

        def = ConfigOptionDef{kOrderDarkKey, coBool};
        def.label = L("From dark to light");
        def.tooltip = L("If checked, it will order the extruder from the darker to the lighter, and the opposite if unchecked. Useful to optimise the purge volume needed when switching colors.");
        def.set_default_value(std::make_unique<ConfigOptionBool>(ConfigOptionBool{ false }));
        m_config.config_def.options[kOrderDarkKey] = def;
        m_config.set_key_value(kOrderDarkKey, def.default_value.get()->clone());

        def = ConfigOptionDef{kSpoolColorsKey, coBool};
        def.label = L("Use real spool colors");
        def.tooltip = L("Uses colors from your spools, defined in the second tab.");
        def.set_default_value(std::make_unique<ConfigOptionBool>(ConfigOptionBool{ false }));
        m_config.config_def.options[kSpoolColorsKey] = def;
        m_config.set_key_value(kSpoolColorsKey, def.default_value.get()->clone());

        def = ConfigOptionDef{kColorComponentKey, coInt};
        def.label = L("Comparator");
        def.tooltip = L("Choose how to compare color, to choose what's merged");
        def.gui_type = ConfigOptionDef::GUIType::i_enum_open;
        def.set_enum_values(ConfigOptionDef::GUIType::i_enum_open, {
            { "0", "RGB" },
            { "1", "RGb" },
            { "2", "Hsv" }
        });
        def.set_default_value(std::make_unique<ConfigOptionInt>(ConfigOptionInt{ 0 }));
        m_config.config_def.options[kColorComponentKey] = def;
        m_config.set_key_value(kColorComponentKey, def.default_value.get()->clone());


        def = ConfigOptionDef{kOriginalKey, coBool};
        def.label = L("Show original colors");
        def.tooltip = L("The preview shows what's going to be generated. Select this option to see the original file instead.");
        def.set_default_value(std::make_unique<ConfigOptionBool>(ConfigOptionBool{ false }));
        m_config.config_def.options[kOriginalKey] = def;
        m_config.set_key_value(kOriginalKey, def.default_value.get()->clone());

        def = ConfigOptionDef{kExtrudersKey, coInt};
        def.label = L("Extruders");
        def.min = 1;
        def.tooltip = L("Shortcut to change the number of extruders quickly. It's not refreshed when the real setting changes, so be careful to don't be confused.");
        def.set_default_value(std::make_unique<ConfigOptionInt>(static_cast<int>(dynamic_cast<TabPrinter*>(this->m_gui_app->get_tab(Preset::TYPE_PRINTER))->m_extruders_count)));
        m_config.config_def.options[kExtrudersKey] = def;
        m_config.set_key_value(kExtrudersKey, def.default_value.get()->clone());

        def = ConfigOptionDef{kAvailableColorsKey, coStrings};
        def.label = L("Available colors");
        def.tooltip = L("Colors available for printing (your spools colors)");
        def.gui_type = ConfigOptionDef::GUIType::color;
        def.set_default_value(std::make_unique<ConfigOptionStrings>(ConfigOptionStrings{ "#0000FF" }));
        m_config.config_def.options[kAvailableColorsKey] = def;
        m_config.set_key_value(kAvailableColorsKey, def.default_value.get()->clone());

        def = ConfigOptionDef{kBackgroundColorKey, coString};
        def.label = L("Background color");
        def.tooltip = L("Color for pillars and base layers.");
        def.gui_type = ConfigOptionDef::GUIType::color;
        def.set_default_value(std::make_unique<ConfigOptionString>( "#FFFFFF" ));
        m_config.config_def.options[kBackgroundColorKey] = def;
        m_config.set_key_value(kBackgroundColorKey, def.default_value.get()->clone());
    }

    boost::filesystem::path path_dir = Slic3r::data_dir();
    path_dir = path_dir / "generator";
    if (!boost::filesystem::exists(path_dir))
        return;
    std::string path = (path_dir / "config.ini").string();

    // 1) Read the complete config file into a boost::property_tree.
    namespace pt = boost::property_tree;
    pt::ptree tree;
    boost::nowide::ifstream ifs;
    bool                    recovered = false;

    try {
        ifs.open(path);
#ifdef WIN32
        // Verify the checksum of the config file without taking just for debugging purpose.
        const AppConfig::ConfigFileInfo config_file_info = AppConfig::check_config_file_and_verify_checksum(ifs);
        if (!config_file_info.correct_checksum) {
            BOOST_LOG_TRIVIAL(info) << "The configuration file " << path <<
                " has a wrong MD5 checksum or the checksum is missing. This may indicate a file corruption or a harmless user edit.";
        }
        if (!config_file_info.correct_checksum && config_file_info.contains_null) {
            BOOST_LOG_TRIVIAL(info) << "The configuration file " + path + " is corrupted, because it is contains null characters.";
            throw Slic3r::CriticalException("The configuration file contains null characters.");
        }
        ifs.seekg(0, boost::nowide::ifstream::beg);
#endif
        try {
            pt::read_ini(ifs, tree);
        }
        catch (pt::ptree_error& ex) {
            throw Slic3r::CriticalException(ex.what());
        }
    }
    catch (Slic3r::CriticalException& ex) {
#ifdef WIN32
        // The configuration file is corrupted, try replacing it with the backup configuration.
        ifs.close();
        recovered = restore_mmu_canvas_config(path, ex.what(), tree);
#else
        BOOST_LOG_TRIVIAL(info) << format(
            R"(Failed to parse configuration file "%1%": %2%)", path, ex.what());
#endif // WIN32
        if (!recovered) {
            // Report the initial error of parsing PrusaSlicer.ini.
            // Error while parsing config file. We'll customize the error message and rethrow to be displayed.
            // ! But to avoid the use of _utf8 (related to use of wxWidgets) 
            // we will rethrow this exception from the place of load() call, if returned value wouldn't be empty
            /*
            throw Slic3r::RuntimeError(
                _utf8(L("Error parsing " SLIC3R_APP_NAME " config file, it is probably corrupted. "
                        "Try to manually delete the file to recover from the error. Your user profiles will not be affected.")) +
                "\n\n" + AppConfig::config_path() + "\n\n" + ex.what());
            */
            BOOST_LOG_TRIVIAL(error) << "Failed to recover MMU canvas configuration after parse error";
        }
    }

    ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Disable);
    // 2) Parse the property_tree, extract the sections and key / value pairs.
    for (const auto& section : tree) {
        if (section.second.empty()) {
            // This may be a top level (no section) entry, or an empty section.
            std::string data = section.second.data();
            if (!data.empty())
                m_config.set_deserialize_nothrow(section.first, data, context);
        }
    }
}

} // namespace GUI
} // namespace Slic3r
