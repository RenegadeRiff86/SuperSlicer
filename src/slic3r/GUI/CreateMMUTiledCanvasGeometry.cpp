#include "CreateMMUTiledCanvas.hpp"

#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/notebook.h>

#include "MainFrame.hpp"
#include <ctime>
#include <cstdio>
#include <cstdlib>

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

indexed_triangle_set its_make_pyramid_inverted(double xd, double yd, double zd, double bezel)
{
    if (bezel == 0) return its_make_cube(xd, yd, zd);

    auto x = float(xd), y = float(yd), z = float(zd), b = float(bezel);

    float x2 = x - b;
    float y2 = y - b;
    if (x2 > b && y2 > b) {
        return {
            { {0, 1, kPyramidVertex2}, {0, kPyramidVertex2, kPyramidVertex3},
              {kPyramidVertex4, kPyramidVertex5, kPyramidVertex6}, {kPyramidVertex4, kPyramidVertex6, kPyramidVertex7},
              {0, kPyramidVertex4, kPyramidVertex7}, {0, kPyramidVertex7, 1},
              {1, kPyramidVertex7, kPyramidVertex6}, {1, kPyramidVertex6, kPyramidVertex2},
              {kPyramidVertex2, kPyramidVertex6, kPyramidVertex5}, {kPyramidVertex2, kPyramidVertex5, kPyramidVertex3},
              {kPyramidVertex4, 0, kPyramidVertex3}, {kPyramidVertex4, kPyramidVertex3, kPyramidVertex5} },
            { {x2,y2,0}, {x2,b, 0}, {b, b, 0}, {b,y2, 0},
              {x, y, z}, {0, y, z}, {0, 0, z}, {x, 0, z} }
        };
    } else if (x2 <= b && y2 <= b) {
        return {
            { {0, kPyramidVertex3, kPyramidVertex2}, {0, kPyramidVertex2, 1},
              {0, 1, kPyramidVertex4}, {0, kPyramidVertex4, kPyramidVertex3}, //4 faces
              {1, kPyramidVertex2, kPyramidVertex3}, {1, kPyramidVertex3, kPyramidVertex4} }, //top
            { {x / kGeometryCenterDivisor, y / kGeometryCenterDivisor, 0},
              {x, y, z}, {0, y, z}, {0, 0, z}, {x, 0, z} }
        };
    } else if (x2 <= b && y2 > b) {
        return {
            { {0, kPyramidVertex3, kPyramidVertex2}, {1, kPyramidVertex5, kPyramidVertex4}, //2 simple faces
              {0, kPyramidVertex2, kPyramidVertex5}, {0, kPyramidVertex5, 1},
              {1, kPyramidVertex4, kPyramidVertex3}, {1, kPyramidVertex3, 0}, //2 double faces
              {kPyramidVertex2, kPyramidVertex3, kPyramidVertex4},
              {kPyramidVertex2, kPyramidVertex4, kPyramidVertex5} }, //top
            { {x / kGeometryCenterDivisor, b, 0}, {x / kGeometryCenterDivisor, y2, 0},
              {0, 0, z},  {x, 0, z}, {x, y, z}, {0, y, z} }
        };
    } else /*if (x2 > b && y2 <= b)*/ {
        return {
            { {0, kPyramidVertex2, kPyramidVertex5}, {1, kPyramidVertex4, kPyramidVertex3}, //2 simple faces
              {0, kPyramidVertex5, kPyramidVertex4}, {0, kPyramidVertex4, 1},
              {1, kPyramidVertex3, kPyramidVertex2}, {1, kPyramidVertex2, 0}, //2 double faces
              {kPyramidVertex2, kPyramidVertex3, kPyramidVertex4},
              {kPyramidVertex2, kPyramidVertex4, kPyramidVertex5} }, //top
            { {b, y / kGeometryCenterDivisor, 0}, {x2, y / kGeometryCenterDivisor, 0},
              {0, 0, z}, {x, 0, z}, {x, y, z}, {0, y, z} }
        };
    }
}

void CreateMMUTiledCanvas::create_geometry(wxCommandEvent& event_args) {

    static_cast<TabPrinter*>(this->m_gui_app->get_tab(Preset::TYPE_PRINTER))->extruders_count_changed(m_config.opt_int(kExtrudersKey));

    //create the base
    Plater* plat = this->m_main_frame->plater();
    plat->take_snapshot(_L("Create Tile"));
    Model& model = plat->model();
    //if (!plat->new_project(L("Tile")))
    //    return;
    model.clear_objects();
    plat->set_project_filename(L("Mosaic"));

    const DynamicPrintConfig* print_config = this->m_gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->m_gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();

    Vec2d offset_dbl = m_config.option<ConfigOptionPoint>("offset")->value;
    wxSize offset(std::min(int(offset_dbl.x()), std::max(0, m_canvas->bmp.GetSize().x - 1)), std::min(int(offset_dbl.y()), std::max(0, m_canvas->bmp.GetSize().y - 1)));
    Vec2d size_dbl = m_config.option<ConfigOptionPoint>("size")->value;
    wxSize size(std::min(int(size_dbl.x()), m_canvas->bmp.GetSize().x - offset.x), std::min(int(size_dbl.y()), m_canvas->bmp.GetSize().y - offset.y));
    Vec2d pixel_size = m_config.option<ConfigOptionPoint>("size_px")->value;
    double separation = m_config.opt_float(kSeparationXyKey);
    double height = m_config.opt_float("height");
    double separation_z = m_config.opt_float(kSeparationZKey);
    double bezel = m_config.opt_float("bezel");
    double border = m_config.opt_float("border");
    //double first_layer_height = print_config->get_computed_value("first_layer_height");
    double layer_height = print_config->get_computed_value("layer_height");
    if (separation_z > height)
        height = separation_z;

    Vec2d total_size{ size.x * (pixel_size.x() + separation) - separation, size.y * (pixel_size.y() + separation) - separation };

    int nb_extruders = dynamic_cast<TabPrinter*>(this->m_gui_app->get_tab(Preset::TYPE_PRINTER))->m_extruders_count;
    bool use_near_color = m_config.option<ConfigOptionBool>(kNearColorKey)->value;
    const int color_algo = m_config.option<ConfigOptionInt>(kColorComponentKey)->value;
    const std::string background_color = m_config.option<ConfigOptionString>(kBackgroundColorKey)->value;
    const bool order_dark = m_config.option<ConfigOptionBool>(kOrderDarkKey)->value;
    const bool use_spool_colors = m_config.option<ConfigOptionBool>(kSpoolColorsKey)->value;

    //sort used color by brightness, to begin dark and lighter and lighter.
    std::sort(m_used_colors.begin(), m_used_colors.end(), [order_dark, use_spool_colors](ColorEntry* ce1, ColorEntry* ce2) {
        wxColour col1 = ce1->get_printed_color(use_spool_colors);
        wxColour col2 = ce2->get_printed_color(use_spool_colors);
        if(order_dark)
            return col1.Red() + col1.Green() + col1.Blue() < col2.Red() + col2.Green() + col2.Blue();
        else
            return col1.Red() + col1.Green() + col1.Blue() > col2.Red() + col2.Green() + col2.Blue();
        });

    std::vector<size_t> objs_idx;
    int idx_extruder_base = 0;
    if (use_near_color) {
        //find extruder
        wxColor color (background_color);
        for (int i = 0; i < int(m_used_colors.size()) && i < nb_extruders; ++i) {
            const bool is_first_candidate = idx_extruder_base <= 0;
            const bool is_nearer = !is_first_candidate &&
                color_dist(color_algo, m_used_colors[i]->get_printed_color(use_spool_colors), color) <
                color_dist(color_algo, m_used_colors[idx_extruder_base - 1]->get_printed_color(use_spool_colors), color);
            idx_extruder_base = is_first_candidate || is_nearer ? 1 + i : idx_extruder_base;
        }
    }
    TriangleMesh mesh(its_make_cube(total_size.x() + (border > 0 ? kBorderSideCount * separation : 0), total_size.y() + (border > 0 ? kBorderSideCount * separation : 0), height - separation_z));
    if (separation_z >= height) {
        //phony volume
        mesh = TriangleMesh(its_make_cube(kPlaceholderCubeEdgeMm, kPlaceholderCubeEdgeMm, kPlaceholderCubeEdgeMm));
    }
    { //this->m_gui_app->obj_list()->load_mesh_object(mesh, _L("Base Tile"), false);
#ifdef _DEBUG
        check_model_ids_validity(model);
#endif /* _DEBUG */
        ModelObject* new_object = model.add_object();
        new_object->name = into_u8(_L("Base Tile"));
        new_object->add_instance(); // each object should have at list one instance

        ModelVolume* new_volume = new_object->add_volume(mesh);
        if (border > 0) {
            new_volume->set_offset(Vec3d{ new_volume->get_offset().x() -separation, new_volume->get_offset().y() -separation, new_volume->get_offset().z() });
        }
        new_object->sort_volumes(wxGetApp().app_config->get("order_volumes") == "1");
        new_volume->name = new_object->name;
        // set a default extruder value
        new_volume->config.set_key_value(kExtruderKey, std::make_unique<ConfigOptionInt>(idx_extruder_base));
        new_object->invalidate_bounding_box();
        //new_object->translate(-bb.center());

        //new_object->instances[0]->set_offset(false ?
        //    to_3d(wxGetApp().plater()->build_volume().bounding_volume2d().center(), -new_object->origin_translation.z()) :
        //    bb.center());

        new_object->ensure_on_bed();

        objs_idx.push_back(model.objects.size() - 1);
    }
    objs_idx.push_back(0);

    if (border > 0) {
        TriangleMesh mesh_N(its_make_cube(total_size.x() + border + kBorderSideCount * separation, border, height));
        ModelVolume* vol_N = model.objects[0]->add_volume(std::move(mesh_N), ModelVolumeType::MODEL_PART, false);
        vol_N->name = "border_N";
        vol_N->set_offset(Vec3d{ -(border + separation) , -(border + separation) , 0 });
        vol_N->config.set_key_value(kExtruderKey, std::make_unique<ConfigOptionInt>(idx_extruder_base));

        TriangleMesh mesh_E(its_make_cube(border, total_size.x() + border + kBorderSideCount * separation, height));
        ModelVolume* vol_E = model.objects[0]->add_volume(std::move(mesh_E), ModelVolumeType::MODEL_PART, false);
        vol_E->name = "border_E";
        vol_E->set_offset(Vec3d{ total_size.x() + (separation) , -(border + separation) , 0 });
        vol_E->config.set_key_value(kExtruderKey, std::make_unique<ConfigOptionInt>(idx_extruder_base));

        TriangleMesh mesh_S(its_make_cube(total_size.x() + border + kBorderSideCount * separation, border, height));
        ModelVolume* vol_S = model.objects[0]->add_volume(std::move(mesh_S), ModelVolumeType::MODEL_PART, false);
        vol_S->name = "border_S";
        vol_S->set_offset(Vec3d{ -(separation) , total_size.y() + (separation) , 0});
        vol_S->config.set_key_value(kExtruderKey, std::make_unique<ConfigOptionInt>(idx_extruder_base));

        TriangleMesh mesh_W(its_make_cube(border, total_size.x() + border + kBorderSideCount * separation, height));
        ModelVolume* vol_W = model.objects[0]->add_volume(std::move(mesh_W), ModelVolumeType::MODEL_PART, false);
        vol_W->name = "border_W";
        vol_W->set_offset(Vec3d{ -(border + separation) , -(separation) , 0 });
        vol_W->config.set_key_value(kExtruderKey, std::make_unique<ConfigOptionInt>(idx_extruder_base));
    }

    wxNativePixelData data(m_canvas->bmp);
    wxNativePixelData::Iterator p(m_canvas->bmp, data);
    if (offset.y > 0)
        p.OffsetY(data, offset.y);
    const auto spool_color = [this, use_spool_colors](const wxColour& source_color) {
        wxColour color = source_color;
        if (!use_spool_colors)
            return color;
        for (const ColorEntry& entry : m_pixel_colors) {
            if (entry.real_color == source_color && entry.widget_spool && !entry.widget_spool->is_auto())
                color = entry.widget_spool->get_print_color()->get_printed_color(true);
        }
        return color;
    };
    const auto add_pixel_volumes = [&](int x, int y, const wxColour& source_color) {
        const wxColour color = spool_color(source_color);
        const int idx_extruder = find_extruder(color);
        if (separation_z > 0) {
            TriangleMesh mesh(its_make_pyramid_inverted(pixel_size.x(), pixel_size.y(), separation_z - layer_height, bezel));
            ModelVolume* vol = model.objects[0]->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
            vol->name = "base_" + std::to_string(offset.x + x) + "_" + std::to_string(offset.y + y);
            vol->set_offset(Vec3d{ x * (pixel_size.x() + separation), total_size.y() - y * (pixel_size.y() + separation) - pixel_size.y(), height - separation_z });
            vol->config.set_key_value(kExtruderKey, std::make_unique<ConfigOptionInt>(idx_extruder_base));
        }

        TriangleMesh mesh(its_make_cube(pixel_size.x(), pixel_size.y(), layer_height));
        ModelVolume* vol = model.objects[0]->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
        vol->name = "tile_" + std::to_string(offset.x + x) + "_" + std::to_string(offset.y + y);
        vol->set_offset(Vec3d{ x * (pixel_size.x() + separation), total_size.y() - y * (pixel_size.y() + separation) - pixel_size.y(), height - layer_height });
        vol->config.set_key_value(kExtruderKey, std::make_unique<ConfigOptionInt>(idx_extruder));
    };

    for (int y = 0; y < size.y; ++y) {
        wxNativePixelData::Iterator rowStart = p;
        if (offset.x > 0)
            p.OffsetX(data, offset.x);
        for (int x = 0; x < size.x; x++, ++p)
            add_pixel_volumes(x, y, wxColour(p.Red(), p.Green(), p.Blue()));
        p = rowStart;
        p.OffsetY(data, 1);
    }

    //for (ModelObject* mo : model.objects) {
    //    objs_idx.push_back(i++);
    //}



    ////open file
    //Plater* plat = this->m_main_frame->plater();
    //Model& model = plat->model();
    //if(cmb_add_replace->GetSelection() == 0)
    //    plat->new_project();
    //std::vector<size_t> objs_idx = plat->load_files(std::vector<std::string>{ object_path.generic_string() }, true, false, false, false);
    //if (objs_idx.empty()) return;
    ////don't save in the temp directory: erase the link to it
    //for (int idx : objs_idx)
    //    model.objects[idx]->input_file = "";
    ///// --- translate ---
    //const DynamicPrintConfig* printerConfig = this->m_gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    //const ConfigOptionPoints* bed_shape = printerConfig->option<ConfigOptionPoints>("bed_shape");
    //Vec2d bed_size = BoundingBoxf(bed_shape->get_values()).size();
    //Vec2d bed_min = BoundingBoxf(bed_shape->get_values()).min;
    //model.objects[objs_idx[0]]->translate({ bed_min.x() + bed_size.x() / 2, bed_min.y() + bed_size.y() / 2, 0 });

    //update colors
    {
        DynamicPrintConfig new_Printer_config = *printer_config; //make a copy
        //{m_impl=L"#800040" m_convertedToChar={m_str=0x0000000000000000 <NULL> m_len=0 } }
        const ConfigOptionStrings* color_conf = printer_config->option<ConfigOptionStrings>("extruder_colour");
        ConfigOptionStrings* new_color_conf = static_cast<ConfigOptionStrings*>(color_conf->clone());
        for(int idx_col = 0; idx_col < int(this->m_used_colors.size()) && idx_col < int(new_color_conf->size()); idx_col++){
            wxColour col = this->m_used_colors[idx_col]->get_printed_color(use_spool_colors);
            new_color_conf->get_at(idx_col) = "#" + int2hex(col.GetRGB());
        }
        new_Printer_config.set_key_value("extruder_colour", new_color_conf);

        this->m_gui_app->get_tab(Preset::TYPE_PRINTER)->load_config(new_Printer_config);
        this->m_main_frame->plater()->on_config_change(new_Printer_config);
        this->m_gui_app->get_tab(Preset::TYPE_PRINTER)->update_dirty();
    }

    //update plater
    plat->changed_objects(objs_idx);
    //update everything, easier to code.
    ObjectList* obj = this->m_gui_app->obj_list();
    obj->update_after_undo_redo();

    if (objs_idx.size() > 1)
        plat->arrange();

    //plat->reslice();
    plat->select_view_3D("3D");

}

} // namespace GUI
} // namespace Slic3r
