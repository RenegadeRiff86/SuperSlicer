#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Surface.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>

#include "FillSmooth.hpp"

namespace Slic3r {

    Polylines FillSmooth::fill_surface(const Surface *surface, const FillParams &params) const
    {
        //ERROR: you shouldn't call that. Default to the rectilinear one.
        printf("FillSmooth::fill_surface() : you call the wrong method (fill_surface instead of fill_surface_extrusion).\n");
        assert(false);
        Polylines polylines_out;
        return polylines_out;
    }

    /// @idx: the index of the step (0 = first step, 1 = second step, ...) The first lay down the volume and the others smoothen the surface.
    void FillSmooth::perform_single_fill(const int idx, ExtrusionEntityCollection &eecroot, const Surface &srf_source,
        const FillParams &params) const {
        if (srf_source.expolygon.empty()) return;
        
        // Save into layer smoothing path.
        ExtrusionEntityCollection *eec = new ExtrusionEntityCollection();
        eec->set_can_sort_reverse(!params.monotonic, !params.monotonic);
        FillParams params_modifided = params;
        if (params.config != NULL && idx > 0) params_modifided.density /= static_cast<float>(params.config->fill_smooth_width.get_abs_value(1));
        else if (params.config != NULL && idx == 0) params_modifided.density *= 1;
        else params_modifided.density *= static_cast<float>(percentWidth[idx]);
        // reduce flow for each increase in density
        params_modifided.flow_mult *= params.density;
        params_modifided.flow_mult /= params_modifided.density;
        // split the flow between steps
        if (params.config != NULL && idx > 0) params_modifided.flow_mult *= static_cast<float>(params.config->fill_smooth_distribution.get_abs_value(1));
        else if (params.config != NULL && idx == 0) params_modifided.flow_mult *= (1.f - static_cast<float>(params.config->fill_smooth_distribution.get_abs_value(1)));
        else params_modifided.flow_mult *= static_cast<float>(percentFlow[idx]);
        //set role
        if (rolePass[idx] != ExtrusionRole::None)
            params_modifided.role = rolePass[idx];

        //choose if we are going to extrude with or without overlap
        if ((params.flow.bridge() && idx == 0) || has_overlap[idx] || this->no_overlap_expolygons.empty()){
            this->fill_expolygon(idx, *eec, srf_source, params_modifided);
        }
        else{
            Surface surfaceNoOverlap(srf_source);
            //use half overlap instead of none.
            ExPolygons half_overlap = offset_ex(this->no_overlap_expolygons, scale_(this->overlap / 2));
            half_overlap = intersection_ex(ExPolygons{ srf_source.expolygon }, half_overlap);
            for (const ExPolygon &poly : half_overlap) {
                if (poly.empty()) continue;
                surfaceNoOverlap.expolygon = poly;
                this->fill_expolygon(idx, *eec, surfaceNoOverlap, params_modifided);
            }
        }
        
        if (eec->entities().empty()) delete eec;
        else eecroot.append(ExtrusionEntitiesPtr{ eec });
    }
    
    void FillSmooth::fill_expolygon(const int idx, ExtrusionEntityCollection &eec, const Surface &srf_to_fill, 
        const FillParams &params_init) const {
        
        FillParams params = params_init;
        params.add_gap_fill = has_gap_fill[idx];
        std::unique_ptr<Fill> f2 = std::unique_ptr<Fill>(Fill::new_from_type(fillPattern[idx]));
        f2->bounding_box = this->bounding_box;
        f2->init_spacing(this->get_spacing(), params);
        f2->layer_id = this->layer_id;
        f2->z = this->z;
        f2->angle = (this->can_angle_cross ? anglePass[idx] : 0) + this->angle;
        // Maximum length of the perimeter segment linking two infill lines.
        f2->link_max_length = this->link_max_length;
        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f2->loop_clipping = this->loop_clipping;

        f2->fill_surface_extrusion(&srf_to_fill, params, eec.set_entities());
    }


    void FillSmooth::fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out) const
    {
        coordf_t init_spacing = this->get_spacing();

        //create root node
        ExtrusionEntityCollection *eecroot = new ExtrusionEntityCollection();
        //you don't want to sort the extrusions: big infill first, small second
        eecroot->set_can_sort_reverse(false, false);

        // first infill
        FillParams first_pass_params = params;
        //if(first_pass_params.role !=  ExtrusionRole::SupportMaterial && first_pass_params.role !=  ExtrusionRole::SupportMaterialInterface)
        //s    first_pass_params.role =  ExtrusionRole::SolidInfill;
        perform_single_fill(0, *eecroot, *surface, first_pass_params);

        //use monotonic for ironing pass
        FillParams monotonic_params = params;
        monotonic_params.monotonic = true;

        //second infill
        if (nbPass > 1){
            perform_single_fill(1, *eecroot, *surface, monotonic_params);
        }

        // third infill
        if (nbPass > 2){
            perform_single_fill(2, *eecroot, *surface, monotonic_params);
        }
        
        if (!eecroot->entities().empty()) {
#ifdef _DEBUGINFO
            eecroot->visit(LoopAssertVisitor());
#endif
            out.push_back(eecroot);
        } else {
            delete eecroot;
        }
    }

} // namespace Slic3r
