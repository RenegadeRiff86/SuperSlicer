
//#define CATCH_CONFIG_DISABLE

#include <catch_main.hpp>

#include <numeric>
#include <sstream>

#include "test_data.hpp" // get access to init_print, etc

#include <libslic3r/Config.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Config.hpp>
#include <libslic3r/GCodeReader.hpp>
#include <libslic3r/Flow.hpp>
#include <libslic3r/libslic3r.h>

#include <libslic3r/Fill/FillBase.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/ExtrusionEntity.hpp>
#include <libslic3r/Layer.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Flow.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/SVG.hpp>
#include <libslic3r/Format/3mf.hpp>

using namespace Slic3r::Test;
using namespace Slic3r;

SCENARIO("Extrusion width specifics", "[!mayfail]") {
    GIVEN("A config with a skirt, brim, some fill density, 3 perimeters, and 1 bottom solid layer and a 20mm cube mesh") {
        // this is a sharedptr
		DynamicPrintConfig config {Slic3r::DynamicPrintConfig::full_print_config()};
        config.set_key_value("skirts", std::make_unique<ConfigOptionInt>(1));
        config.set_key_value("brim_width", std::make_unique<ConfigOptionFloat>(2));
        config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(3));
        config.set_key_value("fill_density", std::make_unique<ConfigOptionPercent>(40));
        config.set_key_value("first_layer_height", std::make_unique<ConfigOptionFloatOrPercent>(100, true));
        config.set_key_value("extruder", std::make_unique<ConfigOptionInt>(0));

        WHEN("first layer width set to 2mm") {
            Slic3r::Model model;
            config.set_key_value("first_layer_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(2.0, false));
            Print print;
            Slic3r::Test::init_print(print, { TestMesh::cube_20x20x20 }, model, &config);
            //std::cout << "model pos: " << model.objects.front()->instances.front()->get_offset().x() << ": " << model.objects.front()->instances.front()->get_offset().x() << "\n";
            //Print print;
            //for (auto* mo : model.objects)
            //    print.auto_assign_extruders(mo);
            //print.apply(model, *config);
            ////std::cout << "print volume: " << print.<< ": " << model.objects().front()->copies().front().x() << "\n";
            //std::string err = print.validate();

            std::vector<double> E_per_mm_bottom;
            std::string gcode_filepath("");
            Slic3r::Test::gcode(gcode_filepath, print);
			GCodeReader parser {Slic3r::GCodeReader()};
            const double layer_height = config.opt_float("layer_height");
            const double first_layer_height = config.get_computed_value("first_layer_height");
            std::string gcode_from_file= read_to_string(gcode_filepath);
            parser.parse_buffer(gcode_from_file, [&E_per_mm_bottom, layer_height, first_layer_height] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
            {
                if (self.z() <= first_layer_height + 0.01) { // only consider first layer
                    if (line.extruding(self) && line.dist_XY(self) > 0) {
                        E_per_mm_bottom.emplace_back(line.dist_E(self) / line.dist_XY(self));
                    }
                }
            });
            THEN(" First layer width applies to everything on first layer.") {
                bool pass = false;
                auto avg_E {std::accumulate(E_per_mm_bottom.cbegin(), E_per_mm_bottom.cend(), 0.0) / static_cast<double>(E_per_mm_bottom.size())};

                pass = (std::count_if(E_per_mm_bottom.cbegin(), E_per_mm_bottom.cend(), [avg_E] (const double& v) { return v == Approx(avg_E); }) == 0);
                REQUIRE(pass == true);
                REQUIRE(E_per_mm_bottom.size() > 0); // make sure it actually passed because of extrusion
            }
            THEN(" First layer width does not apply to upper layer.") {
            }
            clean_file(gcode_filepath, "gcode");
        }
    }
}
// needs gcode export
SCENARIO(" Bridge flow specifics.", "[!mayfail]") {
    GIVEN("A default config with no cooling and a fixed bridge speed, flow ratio and an overhang mesh.") {
        WHEN("bridge_flow_ratio is set to 1.0") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 0.5") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 2.0") {
            THEN("Output flow is as expected.") {
            }
        }
    }
    GIVEN("A default config with no cooling and a fixed bridge speed, flow ratio, fixed extrusion width of 0.4mm and an overhang mesh.") {
        WHEN("bridge_flow_ratio is set to 1.0") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 0.5") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 2.0") {
            THEN("Output flow is as expected.") {
            }
        }
    }
}

/// Test the expected behavior for auto-width, 
/// spacing, etc
SCENARIO("Flow: Flow math for non-bridges", "[!mayfail]") {
    auto width_0 = ConfigOptionFloatOrPercent(0.0, false);
    // spacing is the derived ("phony") half of the width/spacing pair - width above is authoritative.
    auto spacing_0 = ConfigOptionFloatOrPercent(0.0, false);
    spacing_0.set_phony(true);
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
        auto width_1 = ConfigOptionFloatOrPercent(1.0, false);
        auto spacing_1 = ConfigOptionFloatOrPercent(1.0, false);
        spacing_1.set_phony(true);
        float spacing {0.4f};
        float nozzle_diameter {0.4f};
        float bridge_flow {1.0f};
        float layer_height {0.25f};
        float spacing_ratio = 1.0f;

        assert(layer_height < nozzle_diameter);

        // Spacing for non-bridges is has some overlap
        THEN("External perimeter flow has a default spacing fixed to 1.05*nozzle_diameter") {
            Flow flow = Flow::new_from_config_width(frExternalPerimeter, width_0, spacing_0, nozzle_diameter, layer_height, spacing_ratio);
            REQUIRE(flow.spacing() == Approx((1.05f*nozzle_diameter) - layer_height * (1.0 - PI / 4.0)));
        }

        THEN("Internal perimeter flow has a default spacing fixed to 1.125*nozzle_diameter") {
            Flow flow {Flow::new_from_config_width(frPerimeter, width_0, spacing_0, nozzle_diameter, layer_height, spacing_ratio)};
            REQUIRE(flow.spacing() == Approx((1.125*nozzle_diameter) - layer_height * (1.0 - PI / 4.0)));
        }
        THEN("Spacing for supplied width is 0.8927f") {
            Flow flow {Flow::new_from_config_width(frExternalPerimeter, width_1, spacing_1, nozzle_diameter, layer_height, spacing_ratio)};
            REQUIRE(flow.spacing() == Approx(width_1.get_abs_value(1.f) - layer_height * (1.0 - PI / 4.0)));
            flow = Flow::new_from_config_width(frPerimeter, width_1, spacing_1, nozzle_diameter, layer_height, spacing_ratio);
            REQUIRE(flow.spacing() == Approx(width_1.get_abs_value(1.f) - layer_height * (1.0 - PI / 4.0)));
        }
    }
    /// Check the min/max
    GIVEN("Nozzle Diameter of 0.25 with extreme width") {
        float nozzle_diameter {0.25f};
        float layer_height {0.5f};
        float spacing_ratio {1.0f};
        WHEN("layer height is set to 0.15") {
            layer_height = 0.15f;
            THEN("Max width is respected.") {
                auto flow {Flow::new_from_config_width(frPerimeter, width_0, spacing_0, nozzle_diameter, layer_height, spacing_ratio)};
                REQUIRE(flow.width() <= Approx(1.4*nozzle_diameter));
            }
            THEN("Min width is respected") {
                auto flow{ Flow::new_from_config_width(frPerimeter, width_0, spacing_0, nozzle_diameter, layer_height, spacing_ratio) };
                REQUIRE(flow.width() >= Approx(1.05*nozzle_diameter));
            }
        }
        WHEN("Layer height is set to 0.3") {
            layer_height = 0.01f;
            THEN("Max width is respected.") {
                auto flow{ Flow::new_from_config_width(frPerimeter, width_0, spacing_0, nozzle_diameter, layer_height, spacing_ratio) };
                REQUIRE(flow.width() <= Approx(1.4*nozzle_diameter));
            }
            THEN("Min width is respected.") {
                auto flow{ Flow::new_from_config_width(frPerimeter, width_0, spacing_0, nozzle_diameter, layer_height, spacing_ratio) };
                REQUIRE(flow.width() >= Approx(1.05*nozzle_diameter));
            }
        }
    }


    ///// Check for an edge case in the maths where the spacing could be 0; original
    ///// math is 0.99. Slic3r issue #4654
    //GIVEN("Input spacing of 0.414159 and a total width of 2") {
    //    double in_spacing = 0.414159;
    //    double total_width = 2.0;
    //    auto flow {Flow::new_from_spacing(1.0, 0.4, 0.3, false)};
    //    WHEN("solid_spacing() is called") {
    //        double result = flow.solid_spacing(total_width, in_spacing);
    //        THEN("Yielded spacing is greater than 0") {
    //            REQUIRE(result > 0);
    //        }
    //    }
    //}

}

/// Spacing, width calculation for bridge extrusions
SCENARIO("Flow: Flow math for bridges", "[!mayfail]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
        float BRIDGE_EXTRA_SPACING_MULT = 0.f; // not used anymore
        auto width {ConfigOptionFloatOrPercent{1.0, false}};
        auto spacing = ConfigOptionFloatOrPercent(1.0, false);
        float nozzle_diameter {0.4f};
        float spacing_ratio {1.0f};
        float layer_height {0.5f};
        WHEN("via bridging_flow()") {
            auto flow {Flow::bridging_flow(nozzle_diameter, nozzle_diameter)};
            THEN("Bridge width is same as nozzle diameter") {
                REQUIRE(flow.width() == Approx(nozzle_diameter));
            }
            THEN("Bridge spacing is same as nozzle diameter + BRIDGE_EXTRA_SPACING_MULT * nozzle_diameter") {
                REQUIRE(flow.spacing() == Approx(nozzle_diameter + BRIDGE_EXTRA_SPACING_MULT * nozzle_diameter));
            }
        }
        REQUIRE(Flow::bridge_extrusion_spacing(nozzle_diameter) == Approx(nozzle_diameter + BRIDGE_EXTRA_SPACING_MULT * nozzle_diameter));
    }
}

SCENARIO("Flow: stats are okay") {
    DynamicPrintConfig config{Slic3r::DynamicPrintConfig::full_print_config()};
    config.set_key_value("skirts", std::make_unique<ConfigOptionInt>(0));
    config.set_key_value("brim_width", std::make_unique<ConfigOptionFloat>(0));
    config.set_key_value("fill_density", std::make_unique<ConfigOptionPercent>(0));
    config.set_key_value("first_layer_height", std::make_unique<ConfigOptionFloatOrPercent>(0.2, false));
    config.set_key_value("layer_height", std::make_unique<ConfigOptionFloat>(0.2));
    config.set_key_value("top_solid_layers", std::make_unique<ConfigOptionInt>(1000));
    config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(1000));
    config.set_key_value("extruder", std::make_unique<ConfigOptionInt>(0));
    config.set_key_value("nozzle_diameter", std::make_unique<ConfigOptionFloats>(1, 0.5f));
    config.set_key_value("solid_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipRectilinear));
    config.set_key_value("first_layer_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(0.5, false));
    config.set_key_value("solid_infill_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(0.5, false));
    config.set_key_value("top_infill_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(0.5, false));
    config.set_key_value("external_perimeter_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(0.5, false));
    config.set_key_value("perimeter_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(0.5, false));
    config.update_phony({});
    
    double cube_side_spacing = 20;
    std::string name;

    WHEN("20x20x20 cube with only infill")
    {
        // only infill -> the sube border is at the spacing (so extrusion may go a bit outside)
        config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(0));
        name = "no_peri";
    }

    
    WHEN("20x20x20 cube with one perimeter")
    {
        // only infill -> the perimeter width touch the border, so it has less fill than with only infill.
        // also, set seam_gap to 0 to avoid removing bits of the perimeters
        cube_side_spacing = 20 - 0.2/*height*/ * float(1. - 0.25 * PI);
        config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(1));
        auto seam_gap = std::make_unique<ConfigOptionFloatsOrPercents>(1, FloatOrPercent{0,false});
        seam_gap->set_is_extruder_size();
        config.set_key_value("seam_gap", std::move(seam_gap));
        
        name = "one_peri";
    }

    WHEN("20x20x20 cube with three perimeters")
    {
        // only infill -> the perimeter width touch the border, so it has less fill than with only infill.
        // also, set seam_gap to 0 to avoid removing bits of the perimeters
        cube_side_spacing = 20 - 0.2/*height*/ * float(1. - 0.25 * PI);
        config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(3));
        auto seam_gap = std::make_unique<ConfigOptionFloatsOrPercents>(1, FloatOrPercent{0,false});
        seam_gap->set_is_extruder_size();
        config.set_key_value("seam_gap", std::move(seam_gap));
        
        name = "three_peri";
    }
    
    WHEN("20x20x20 cube with three arachne perimeters")
    {
        // only infill -> the perimeter width touch the border, so it has less fill than with only infill.
        // also, set seam_gap to 0 to avoid removing bits of the perimeters
        cube_side_spacing = 20 - 0.2/*height*/ * float(1. - 0.25 * PI);
        config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(3));
        config.set_key_value("perimeter_generator", std::make_unique<ConfigOptionEnum<PerimeterGeneratorType>>(PerimeterGeneratorType::Arachne));
        auto seam_gap = std::make_unique<ConfigOptionFloatsOrPercents>(1, FloatOrPercent{0, false});
        seam_gap->set_is_extruder_size();
        config.set_key_value("seam_gap", std::move(seam_gap));
        name = "arachne_three_peri";
    }

    Slic3r::Model model;
    Print         print;
    Slic3r::Test::init_print(print, {TestMesh::cube_20x20x20}, model, &config);
    const double volume = (cube_side_spacing*cube_side_spacing*20);
    const double volume_layer = (cube_side_spacing*cube_side_spacing*0.2);
    print.process();
    REQUIRE(print.get_object(0)->get_layer(0)->height == Approx(0.2));
    REQUIRE(print.get_object(0)->get_layer(1)->height == Approx(0.2));
        
    std::string gcode_filepath{ "" };
    Slic3r::Test::gcode(gcode_filepath, print);
    std::string gcode_from_file = read_to_string(gcode_filepath);
    Slic3r::PrintStatistics &stats = print.print_statistics();
    //string[] lineArray = gcode_from_file
    GCodeReader parser;
    double volume_extruded = 0;
    //int idx = 0;
    int step = 0;
    double volume_perimeter_extruded = 0;
    double volume_infill_extruded = 0;
    double volume_other_extruded = 0;
    // Records every unexpected TYPE: seen, so a failure names the culprit instead of just a number.
    std::string other_types;
    // add remaining time lines where needed
    parser.parse_buffer(gcode_from_file,
        [&](GCodeReader& reader, const GCodeReader::GCodeLine& line)
    {
        // These must match gcode_extrusion_role_to_string() in ExtrusionRole.cpp exactly - there is
        // no "Internal perimeter" role, internal loops are emitted as plain "Perimeter".
        // Walls: everything the perimeter generator emits, including the gap fill and thin walls it
        // produces to close what the loops cannot reach.
        if (line.comment() == "TYPE:External perimeter" || line.comment() == "TYPE:Perimeter"
            || line.comment() == "TYPE:Overhang perimeter" || line.comment() == "TYPE:Gap fill"
            || line.comment() == "TYPE:Thin wall")
            step = 1;
        else if (line.comment() == "TYPE:Solid infill" || line.comment() == "TYPE:Top solid infill"
            || line.comment() == "TYPE:Internal infill" || line.comment() == "TYPE:Bridge infill"
            || line.comment() == "TYPE:Internal bridge infill" || line.comment() == "TYPE:Ironing")
            step = 2;
        else if (boost::starts_with(line.comment(), "TYPE:")) {
            step = 0;
            const std::string type{line.comment()};
            if (other_types.find(type) == std::string::npos)
                other_types += type + " ";
        }
        if (line.cmd_is("G1"))
        {
            if (line.dist_E(reader) > 0 && line.dist_XY(reader) > 0) {
                volume_extruded += line.dist_E(reader)*(PI*1.75*1.75 / 4.);
                if (step == 0) volume_other_extruded += line.dist_E(reader)*(PI*1.75*1.75 / 4.);
                else if (step == 1) volume_perimeter_extruded += line.dist_E(reader)*(PI*1.75*1.75 / 4.);
                else if (step == 2) volume_infill_extruded += line.dist_E(reader)*(PI*1.75*1.75 / 4.);
            }
        }
    });
    INFO("unexpected extrusion types: " << other_types);
    INFO("nominal " << volume << " vs gcode-extruded " << volume_extruded
         << " (perimeter " << volume_perimeter_extruded
         << " + infill " << volume_infill_extruded << ")");
    REQUIRE(volume_other_extruded == 0);

    REQUIRE(print.get_object(0)->layers().size() == 100);
    for (size_t layer_id = 0; layer_id < 100; layer_id++) {
        const auto *region = print.get_object(0)->get_layer(layer_id)->regions()[0];
        // Gap fill and thin walls live in thin_fills(), not perimeters() - they come out of the
        // perimeter generator, so count them as wall (same split as the TYPE: parsing above).
        // Ironing is its own collection as well; omitting either undercounts the layer.
        double volumeExtrPerimeter = ExtrusionVolume{}.get(region->perimeters())
                                   + ExtrusionVolume{}.get(region->thin_fills());
        double volumeExtrInfill = ExtrusionVolume{}.get(region->fills())
                                + ExtrusionVolume{}.get(region->ironings());
        REQUIRE(volume_layer == Approx(volumeExtrInfill + volumeExtrPerimeter));
        // no perimeter -> no fill_no_overlap_expolygons
        REQUIRE( (config.option("perimeters")->get_int() == 0) == (volumeExtrPerimeter == 0));
        REQUIRE( (volumeExtrPerimeter == 0) == print.get_object(0)->get_layer(0)->regions()[0]->fill_no_overlap_expolygons().empty());
    }
    
    std::cout<<name<<" : "<<volume<<" mm3\n";
    REQUIRE(volume == Approx(volume_infill_extruded+volume_perimeter_extruded));
    REQUIRE(volume == Approx(volume_extruded));
    REQUIRE(volume == Approx(stats.total_extruded_volume));
    clean_file(gcode_filepath, "gcode");
    Slic3r::store_3mf((name + std::string(".3mf")).c_str(), &model, &print.full_print_config(), OptionStore3mf{});
}

