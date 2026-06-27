/*  ADMesh -- process triangulated solid meshes
 *  Copyright (C) 1995, 1996  Anthony D. Martin <amartin@engr.csulb.edu>
 *  Copyright (C) 2013, 2014  several contributors, see AUTHORS
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.

 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.

 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *  Questions, comments, suggestions, etc to
 *           https://github.com/admesh/admesh/issues
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <boost/log/trivial.hpp>

#include "stl.h"

// A triangular facet always has 3 vertices / edges.
static constexpr int VERTICES_PER_FACET = 3;
// Coordinate axis indices into an stl_vertex / Eigen vector.
static constexpr int X_AXIS = 0;
static constexpr int Y_AXIS = 1;
static constexpr int Z_AXIS = 2;
static constexpr int AXIS_COUNT = 3; // number of spatial axes (X, Y, Z)

void stl_verify_neighbors(stl_file *stl)
{
	stl->stats.backwards_edges = 0;

	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
		for (int j = 0; j < VERTICES_PER_FACET; ++ j) {
			struct stl_edge {
				stl_vertex p1;
				stl_vertex p2;
				int        facet_number;
			};
			stl_edge edge_a;
			edge_a.p1 = stl->facet_start[i].vertex[j];
			edge_a.p2 = stl->facet_start[i].vertex[(j + 1) % VERTICES_PER_FACET];
			int neighbor = stl->neighbors_start[i].neighbor[j];
			if (neighbor == -1)
				continue; // this edge has no neighbor... Continue.
			int vnot = stl->neighbors_start[i].which_vertex_not[j];
			stl_edge edge_b;
			if (vnot < VERTICES_PER_FACET) {
				edge_b.p1 = stl->facet_start[neighbor].vertex[(vnot + 2) % VERTICES_PER_FACET];
				edge_b.p2 = stl->facet_start[neighbor].vertex[(vnot + 1) % VERTICES_PER_FACET];
			} else {
				stl->stats.backwards_edges += 1;
				edge_b.p1 = stl->facet_start[neighbor].vertex[(vnot + 1) % VERTICES_PER_FACET];
				edge_b.p2 = stl->facet_start[neighbor].vertex[(vnot + 2) % VERTICES_PER_FACET];
			}
			if (edge_a.p1 != edge_b.p1 || edge_a.p2 != edge_b.p2) {
				// These edges should match but they don't.  Print results.
				BOOST_LOG_TRIVIAL(info) << "edge " << j << " of facet " << i << " doesn't match edge " << (vnot + 1) << " of facet " << neighbor;
				stl_write_facet(stl, (char*)"first facet", i);
				stl_write_facet(stl, (char*)"second facet", neighbor);
			}
		}
	}
}

void stl_translate(stl_file *stl, float x, float y, float z)
{
	stl_vertex new_min(x, y, z);
	stl_vertex shift = new_min - stl->stats.min;
	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
		for (int j = 0; j < VERTICES_PER_FACET; ++ j)
	  		stl->facet_start[i].vertex[j] += shift;
	stl->stats.min = new_min;
	stl->stats.max += shift;
}

/* Translates the stl by x,y,z, relatively from wherever it is currently */
void stl_translate_relative(stl_file *stl, float x, float y, float z)
{
	stl_vertex shift(x, y, z);
	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
		for (int j = 0; j < VERTICES_PER_FACET; ++ j)
	  		stl->facet_start[i].vertex[j] += shift;
	stl->stats.min += shift;
	stl->stats.max += shift;
}

void stl_scale_versor(stl_file *stl, const stl_vertex &versor)
{
	// Scale extents.
	auto s = versor.array();
	stl->stats.min.array() *= s;
	stl->stats.max.array() *= s;
	// Scale size.
	stl->stats.size.array() *= s;
	// Scale volume.
	if (stl->stats.volume > 0.0)
		stl->stats.volume *= versor(X_AXIS) * versor(Y_AXIS) * versor(Z_AXIS);
	// Scale the mesh.
	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
		for (int j = 0; j < VERTICES_PER_FACET; ++ j)
	  		stl->facet_start[i].vertex[j].array() *= s;
}

static void calculate_normals(stl_file *stl) 
{
	stl_normal normal;
	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
		stl_calculate_normal(normal, &stl->facet_start[i]);
		stl_normalize_vector(normal);
		stl->facet_start[i].normal = normal;
	}
}

static inline void rotate_point_2d(float &x, float &y, const double c, const double s)
{
	double xold = x;
	double yold = y;
	x = float(c * xold - s * yold);
	y = float(s * xold + c * yold);
}

// Degrees in a half turn (? radians); used to convert a degree angle to radians.
static constexpr double HALF_CIRCLE_DEGREES = 180.0;

void stl_rotate_x(stl_file *stl, float angle)
{
	double radian_angle = (angle / HALF_CIRCLE_DEGREES) * M_PI;
	double c = cos(radian_angle);
	double s = sin(radian_angle);
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
    	for (int j = 0; j < VERTICES_PER_FACET; ++ j)
      		rotate_point_2d(stl->facet_start[i].vertex[j](Y_AXIS), stl->facet_start[i].vertex[j](Z_AXIS), c, s);
  	stl_get_size(stl);
  	calculate_normals(stl);
}

void stl_rotate_y(stl_file *stl, float angle)
{
	double radian_angle = (angle / HALF_CIRCLE_DEGREES) * M_PI;
	double c = cos(radian_angle);
	double s = sin(radian_angle);
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
    	for (int j = 0; j < VERTICES_PER_FACET; ++ j)
			rotate_point_2d(stl->facet_start[i].vertex[j](Z_AXIS), stl->facet_start[i].vertex[j](X_AXIS), c, s);
  	stl_get_size(stl);
  	calculate_normals(stl);
}

void stl_rotate_z(stl_file *stl, float angle)
{
	double radian_angle = (angle / HALF_CIRCLE_DEGREES) * M_PI;
	double c = cos(radian_angle);
	double s = sin(radian_angle);
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
    	for (int j = 0; j < VERTICES_PER_FACET; ++ j)
      		rotate_point_2d(stl->facet_start[i].vertex[j](X_AXIS), stl->facet_start[i].vertex[j](Y_AXIS), c, s);
  	stl_get_size(stl);
  	calculate_normals(stl);
}

void its_rotate_x(indexed_triangle_set &its, float angle)
{
	double radian_angle = (angle / HALF_CIRCLE_DEGREES) * M_PI;
	double c = cos(radian_angle);
	double s = sin(radian_angle);
	for (stl_vertex &v : its.vertices)
		rotate_point_2d(v(Y_AXIS), v(Z_AXIS), c, s);
}

void its_rotate_y(indexed_triangle_set& its, float angle)
{
	double radian_angle = (angle / HALF_CIRCLE_DEGREES) * M_PI;
	double c = cos(radian_angle);
	double s = sin(radian_angle);
	for (stl_vertex& v : its.vertices)
		rotate_point_2d(v(Z_AXIS), v(X_AXIS), c, s);
}

void its_rotate_z(indexed_triangle_set& its, float angle)
{
	double radian_angle = (angle / HALF_CIRCLE_DEGREES) * M_PI;
	double c = cos(radian_angle);
	double s = sin(radian_angle);
	for (stl_vertex& v : its.vertices)
		rotate_point_2d(v(X_AXIS), v(Y_AXIS), c, s);
}

void stl_get_size(stl_file *stl)
{
  	if (stl->stats.number_of_facets == 0)
  		return;
  	stl->stats.min = stl->facet_start[0].vertex[0];
  	stl->stats.max = stl->stats.min;
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
  		const stl_facet &face = stl->facet_start[i];
    	for (int j = 0; j < VERTICES_PER_FACET; ++ j) {
      		stl->stats.min = stl->stats.min.cwiseMin(face.vertex[j]);
      		stl->stats.max = stl->stats.max.cwiseMax(face.vertex[j]);
    	}
  	}
  	stl->stats.size = stl->stats.max - stl->stats.min;
  	stl->stats.bounding_diameter = stl->stats.size.norm();
}

// Reflection factor: multiplying a coordinate by this mirrors the mesh across the
// plane perpendicular to that axis (used by the stl_mirror_* helpers below).
static constexpr float MIRROR_SCALE = -1.0f;

void stl_mirror_xy(stl_file *stl)
{
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
    	for (int j = 0; j < VERTICES_PER_FACET; ++ j)
      		stl->facet_start[i].vertex[j](Z_AXIS) *= MIRROR_SCALE;
	float temp_size = stl->stats.min(Z_AXIS);
	stl->stats.min(Z_AXIS) = stl->stats.max(Z_AXIS);
	stl->stats.max(Z_AXIS) = temp_size;
	stl->stats.min(Z_AXIS) *= MIRROR_SCALE;
	stl->stats.max(Z_AXIS) *= MIRROR_SCALE;
	stl_reverse_all_facets(stl);
	stl->stats.facets_reversed -= stl->stats.number_of_facets;  /* for not altering stats */
}

void stl_mirror_yz(stl_file *stl)
{
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
    	for (int j = 0; j < VERTICES_PER_FACET; j++)
      		stl->facet_start[i].vertex[j](X_AXIS) *= MIRROR_SCALE;
	float temp_size = stl->stats.min(X_AXIS);
	stl->stats.min(X_AXIS) = stl->stats.max(X_AXIS);
	stl->stats.max(X_AXIS) = temp_size;
	stl->stats.min(X_AXIS) *= MIRROR_SCALE;
	stl->stats.max(X_AXIS) *= MIRROR_SCALE;
	stl_reverse_all_facets(stl);
	stl->stats.facets_reversed -= stl->stats.number_of_facets;  /* for not altering stats */
}

void stl_mirror_xz(stl_file *stl)
{
	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
		for (int j = 0; j < VERTICES_PER_FACET; ++ j)
			stl->facet_start[i].vertex[j](Y_AXIS) *= MIRROR_SCALE;
	float temp_size = stl->stats.min(Y_AXIS);
	stl->stats.min(Y_AXIS) = stl->stats.max(Y_AXIS);
	stl->stats.max(Y_AXIS) = temp_size;
	stl->stats.min(Y_AXIS) *= MIRROR_SCALE;
	stl->stats.max(Y_AXIS) *= MIRROR_SCALE;
	stl_reverse_all_facets(stl);
	stl->stats.facets_reversed -= stl->stats.number_of_facets;  // for not altering stats
}

static float get_area(const stl_facet *facet)
{
	/* cast to double before calculating cross product because large coordinates
	 can result in overflowing product
	(bad area is responsible for bad volume and bad facets reversal) */
	double cross[VERTICES_PER_FACET][AXIS_COUNT] = {};
	for (int i = 0; i < VERTICES_PER_FACET; i++) {
		cross[i][X_AXIS]=((static_cast<double>(facet->vertex[i](Y_AXIS)) * static_cast<double>(facet->vertex[(i + 1) % VERTICES_PER_FACET](Z_AXIS))) -
	             	 (static_cast<double>(facet->vertex[i](Z_AXIS)) * static_cast<double>(facet->vertex[(i + 1) % VERTICES_PER_FACET](Y_AXIS))));
		cross[i][Y_AXIS]=((static_cast<double>(facet->vertex[i](Z_AXIS)) * static_cast<double>(facet->vertex[(i + 1) % VERTICES_PER_FACET](X_AXIS))) -
	             	 (static_cast<double>(facet->vertex[i](X_AXIS)) * static_cast<double>(facet->vertex[(i + 1) % VERTICES_PER_FACET](Z_AXIS))));
		cross[i][Z_AXIS]=((static_cast<double>(facet->vertex[i](X_AXIS)) * static_cast<double>(facet->vertex[(i + 1) % VERTICES_PER_FACET](Y_AXIS))) -
	             	 (static_cast<double>(facet->vertex[i](Y_AXIS)) * static_cast<double>(facet->vertex[(i + 1) % VERTICES_PER_FACET](X_AXIS))));
	}

	// Sum each axis component of the per-vertex cross products.
	stl_normal sum = stl_normal::Zero();
	for (int v = 0; v < VERTICES_PER_FACET; ++v) {
		sum(X_AXIS) += cross[v][X_AXIS];
		sum(Y_AXIS) += cross[v][Y_AXIS];
		sum(Z_AXIS) += cross[v][Z_AXIS];
	}

	// Recompute a normalized geometric normal in case facet->normal is stale.
	// The accumulated sum still carries the signed area magnitude.
	stl_normal n;
	stl_calculate_normal(n, facet);
	stl_normalize_vector(n);
	return 0.5f * n.dot(sum);
}

static float get_volume(stl_file *stl)
{
  	// Choose a point, any point as the reference.
  	stl_vertex p0 = stl->facet_start[0].vertex[0];
  	float volume = 0.f;
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
    	// Do dot product to get distance from point to plane.
    	float height = stl->facet_start[i].normal.dot(stl->facet_start[i].vertex[0] - p0);
    	float area   = get_area(&stl->facet_start[i]);
    	volume += (area * height) / 3.0f;
  	}
  	return volume;
}

void stl_calculate_volume(stl_file *stl)
{
  	stl->stats.volume = get_volume(stl);
  	if (stl->stats.volume < 0.0) {
    	stl_reverse_all_facets(stl);
    	stl->stats.volume = -stl->stats.volume;
  	}
}

void stl_repair(
	stl_file *stl,
	bool fixall_flag,
	bool exact_flag,
	bool tolerance_flag,
	float tolerance,
	bool increment_flag,
	float increment,
	bool nearby_flag,
	int iterations,
	bool remove_unconnected_flag,
	bool fill_holes_flag,
	bool normal_directions_flag,
	bool normal_values_flag,
	bool reverse_all_flag,
	bool verbose_flag)
{
	if (exact_flag || fixall_flag || nearby_flag || remove_unconnected_flag || fill_holes_flag || normal_directions_flag) {
		if (verbose_flag)
		  	printf("Checking exact...\n");
		exact_flag = true;
		stl_check_facets_exact(stl);
		stl->stats.facets_w_1_bad_edge = (stl->stats.connected_facets_2_edge - stl->stats.connected_facets_3_edge);
		stl->stats.facets_w_2_bad_edge = (stl->stats.connected_facets_1_edge - stl->stats.connected_facets_2_edge);
		stl->stats.facets_w_3_bad_edge = (stl->stats.number_of_facets - stl->stats.connected_facets_1_edge);
	}

  	if (nearby_flag || fixall_flag) {
    	if (! tolerance_flag)
      		tolerance = stl->stats.shortest_edge;
 	   	if (! increment_flag)
      		increment = stl->stats.bounding_diameter / 10000.0;
    }

	if (stl->stats.connected_facets_3_edge < int(stl->stats.number_of_facets)) {
	  	int last_edges_fixed = 0;
	  	for (int i = 0; i < iterations; ++ i) {
	    	if (stl->stats.connected_facets_3_edge < int(stl->stats.number_of_facets)) {
	      		if (verbose_flag)
	        		printf("Checking nearby. Tolerance= %f Iteration=%d of %d...", tolerance, i + 1, iterations);
	      		stl_check_facets_nearby(stl, tolerance);
	      		if (verbose_flag)
	        		printf("  Fixed %d edges.\n", stl->stats.edges_fixed - last_edges_fixed);
	      		last_edges_fixed = stl->stats.edges_fixed;
	      		tolerance += increment;
	    	} else {
	    		if (verbose_flag)
	        		printf("All facets connected.  No further nearby check necessary.\n");
		      	break;
		    }
	  	}
	} else if (verbose_flag)
	    printf("All facets connected.  No nearby check necessary.\n");

	if (remove_unconnected_flag || fixall_flag || fill_holes_flag) {
		if (stl->stats.connected_facets_3_edge < int(stl->stats.number_of_facets)) {
	  		if (verbose_flag)
	    		printf("Removing unconnected facets...\n");
	  		stl_remove_unconnected_facets(stl);
		} else if (verbose_flag)
	    	printf("No unconnected need to be removed.\n");
	}

	if (fill_holes_flag || fixall_flag) {
		if (stl->stats.connected_facets_3_edge < int(stl->stats.number_of_facets)) {
	  		if (verbose_flag)
	    		printf("Filling holes...\n");
	  		stl_fill_holes(stl);
		} else if (verbose_flag)
	    	printf("No holes need to be filled.\n");
	}

	if (reverse_all_flag) {
		if (verbose_flag)
	  		printf("Reversing all facets...\n");
		stl_reverse_all_facets(stl);
	}

	if (normal_directions_flag || fixall_flag) {
		if (verbose_flag)
	  		printf("Checking normal directions...\n");
		stl_fix_normal_directions(stl);
	}

	if (normal_values_flag || fixall_flag) {
		if (verbose_flag)
	  		printf("Checking normal values...\n");
		stl_fix_normal_values(stl);
	}

  	// Always calculate the volume.  It shouldn't take too long.
	if (verbose_flag)
		printf("Calculating volume...\n");
	stl_calculate_volume(stl);

	if (exact_flag) {
		if (verbose_flag)
	  		printf("Verifying neighbors...\n");
		stl_verify_neighbors(stl);
	}
}
