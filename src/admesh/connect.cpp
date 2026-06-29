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

#include <algorithm>
#include <vector>

#include <boost/predef/other/endian.h>
#include <boost/log/trivial.hpp>
// Boost pool: Don't use mutexes to synchronize memory allocation.
#define BOOST_POOL_NO_MT
#include <boost/pool/object_pool.hpp>

#include "stl.h"

// Coprime divisors that scatter the six key words when hashing an edge.
static constexpr uint32_t kHashDivA = 11;
static constexpr uint32_t kHashDivB = 7;
static constexpr uint32_t kHashDivC = 3;
// IEEE-754 bit pattern of -0.0f, normalized to +0.0 so equal edges compare equal.
static constexpr uint32_t kNegativeZeroBits = 0x80000000u;
// Number of facet edges connected to a neighbor (0..3 of the three edges).
enum { kZeroEdges, kOneEdge, kTwoEdges, kThreeEdges };
// Coordinate axis indices into an stl_vertex / Eigen vector.
enum { X_AXIS, Y_AXIS, Z_AXIS };
// Local vertex indices within a triangular facet (0, 1, 2).
enum { kV0, kV1, kV2 };

struct HashEdge {
    // Key of a hash edge: sorted vertices of the edge.
    uint32_t       key[6];
    // Compare two keys.
    bool operator==(const HashEdge &rhs) const { return memcmp(key, rhs.key, sizeof(key)) == 0; }
    bool operator!=(const HashEdge &rhs) const { return ! (*this == rhs); }
    int  hash(int M) const { return ((key[0] / kHashDivA + key[1] / kHashDivB + key[Z_AXIS] / kHashDivC) ^ (key[STL_VERTICES_PER_FACET] / kHashDivA  + key[4] / kHashDivB + key[5] / kHashDivC)) % M; }

    // Index of a facet owning this edge.
    int        facet_number;
    // Index of this edge inside the facet with an index of facet_number.
    // If this edge is stored backwards, which_edge is increased by 3.
    int        which_edge;
    HashEdge  *next;

    void load_exact(stl_file *stl, const stl_vertex *a, const stl_vertex *b)
    {
        {
            stl_vertex diff = (*a - *b).cwiseAbs();
            float max_diff = std::max(diff(X_AXIS), std::max(diff(Y_AXIS), diff(Z_AXIS)));
            stl->stats.shortest_edge = std::min(max_diff, stl->stats.shortest_edge);
        }

        // Ensure identical vertex ordering of equal edges.
        // This method is numerically robust.
        if (vertex_lower(*a, *b)) {
        } else {
            // This edge is loaded backwards.
            std::swap(a, b);
            this->which_edge += STL_VERTICES_PER_FACET;
        }
        memcpy(&this->key[0], a->data(), sizeof(stl_vertex));
        memcpy(&this->key[STL_VERTICES_PER_FACET], b->data(), sizeof(stl_vertex));
        // Switch negative zeros to positive zeros, so memcmp will consider them to be equal.
        // -0.0f has the IEEE-754 bit pattern 0x80000000 in the native word on every platform,
        // so one word comparison replaces the old per-byte, endian-specific test.
        for (size_t i = 0; i < 6; ++ i)
            if (this->key[i] == kNegativeZeroBits)
                this->key[i] = 0;
    }

    bool load_nearby(const stl_file *stl, const stl_vertex &a, const stl_vertex &b, float tolerance)
    {
        // Index of a grid cell spaced by tolerance.
        typedef Eigen::Matrix<int32_t,  3, 1, Eigen::DontAlign> Vec3i;
        Vec3i vertex1 = ((a - stl->stats.min) / tolerance).cast<int32_t>();
        Vec3i vertex2 = ((b - stl->stats.min) / tolerance).cast<int32_t>();
        static_assert(sizeof(Vec3i) == 12, "size of Vec3i incorrect");

        if (vertex1 == vertex2)
            // Both vertices hash to the same value
            return false;

        // Ensure identical vertex ordering of edges, which vertices land into equal grid cells.
        // This method is numerically robust.
        if ((vertex1[X_AXIS] != vertex2[X_AXIS]) ? 
            (vertex1[X_AXIS] < vertex2[X_AXIS]) : 
            ((vertex1[Y_AXIS] != vertex2[Y_AXIS]) ? 
                (vertex1[Y_AXIS] < vertex2[Y_AXIS]) : 
                (vertex1[Z_AXIS] < vertex2[Z_AXIS]))) {
            memcpy(&this->key[0], vertex1.data(), sizeof(stl_vertex));
            memcpy(&this->key[STL_VERTICES_PER_FACET], vertex2.data(), sizeof(stl_vertex));
        } else {
            memcpy(&this->key[0], vertex2.data(), sizeof(stl_vertex));
            memcpy(&this->key[STL_VERTICES_PER_FACET], vertex1.data(), sizeof(stl_vertex));
            this->which_edge += STL_VERTICES_PER_FACET; /* this edge is loaded backwards */
        }
        return true;
    }

private:
    inline bool vertex_lower(const stl_vertex &a, const stl_vertex &b) {
        return (a(X_AXIS) != b(X_AXIS)) ? (a(X_AXIS) < b(X_AXIS)) :
               ((a(Y_AXIS) != b(Y_AXIS)) ? (a(Y_AXIS) < b(Y_AXIS)) : (a(Z_AXIS) < b(Z_AXIS)));
    }
};

struct HashTableEdges {
    HashTableEdges(size_t number_of_faces) {
        this->M = static_cast<int>(hash_size_from_nr_faces(number_of_faces));
        this->heads.assign(this->M, nullptr);
        this->tail = pool.construct();
        this->tail->next = this->tail;
        for (int i = 0; i < this->M; ++ i)
            this->heads[i] = this->tail;
    }
    ~HashTableEdges() {
#ifndef NDEBUG
        for (int i = 0; i < this->M; ++ i)
            for (HashEdge *temp = this->heads[i]; temp != this->tail; temp = temp->next)
                ++ this->freed;
        this->tail = nullptr;
#endif /* NDEBUG */
    }

    void insert_edge_exact(stl_file *stl, const HashEdge &edge)
    {
        this->insert_edge(stl, edge, [stl](const HashEdge& edge1, const HashEdge& edge2) { record_neighbors(stl, edge1, edge2); });
    }

    void insert_edge_nearby(stl_file *stl, const HashEdge &edge)
    {
        this->insert_edge(stl, edge, [stl](const HashEdge& edge1, const HashEdge& edge2) { match_neighbors_nearby(stl, edge1, edge2); });
    }

    // Hash table on edges
    std::vector<HashEdge*> 	heads;
    HashEdge* 				tail;
    int           			M;
    boost::object_pool<HashEdge> pool;

#ifndef NDEBUG
    size_t 					malloced   	= 0;
    size_t 					freed 	  	= 0;
    size_t 					collisions 	= 0;
#endif /* NDEBUG */

private:
    static inline size_t hash_size_from_nr_faces(const size_t nr_faces)
    {
        // Good primes for addressing a cca. 30 bit space.
        // https://planetmath.org/goodhashtableprimes
        static std::vector<uint32_t> primes{ 98317, 196613, 393241, 786433, 1572869, 3145739, 6291469, 12582917, 25165843, 50331653, 100663319, 201326611, 402653189, 805306457, 1610612741 };
        // Find a prime number for 50% filling of the shared triangle edges in the mesh.
        auto it = std::upper_bound(primes.begin(), primes.end(), nr_faces * STL_VERTICES_PER_FACET * 2 - 1);
        return (it == primes.end()) ? primes.back() : *it;
    }


    // MatchNeighbors(stl_file *stl, const HashEdge &edge_a, const HashEdge &edge_b)
    template<typename MatchNeighbors>
    void insert_edge(stl_file *stl, const HashEdge &edge, MatchNeighbors match_neighbors)
    {
        int       chain_number = edge.hash(this->M);
        HashEdge *link         = this->heads[chain_number];
        if (link == this->tail) {
            // This list doesn't have any edges currently in it.  Add this one.
            HashEdge *new_edge = pool.construct(edge);
#ifndef NDEBUG
            ++ this->malloced;
#endif /* NDEBUG */
            new_edge->next = this->tail;
            this->heads[chain_number] = new_edge;
        } else if (edges_equal(edge, *link)) {
            // This is a match.  Record result in neighbors list.
            match_neighbors(edge, *link);
            // Delete the matched edge from the list.
            this->heads[chain_number] = link->next;
            // pool.destroy(link);
#ifndef NDEBUG
            ++ this->freed;
#endif /* NDEBUG */
        } else {
            // Continue through the rest of the list.
            for (;;) {
                if (link->next == this->tail) {
                    // This is the last item in the list. Append edge to the hash bucket.
                    HashEdge *new_edge = pool.construct();
#ifndef NDEBUG
                    ++ this->malloced;
#endif /* NDEBUG */
                    *new_edge = edge;
                    new_edge->next = this->tail;
                    link->next = new_edge;
#ifndef NDEBUG
                    ++ this->collisions;
#endif /* NDEBUG */
                    break;
                }
                if (edges_equal(edge, *link->next)) {
                    // This is a match.  Record result in neighbors list.
                    match_neighbors(edge, *link->next);
                    // Delete the matched edge from the list.
                    HashEdge *temp = link->next;
                    link->next = link->next->next;
                    // pool.destroy(temp);
#ifndef NDEBUG
                    ++ this->freed;
#endif /* NDEBUG */
                    break;
                }
                // This is not a match.  Go to the next link.
                link = link->next;
#ifndef NDEBUG
                ++ this->collisions;
#endif /* NDEBUG */
            }
        }
    }

    // Edges equal for hashing. Edgesof different facet are allowed to be matched.
    static inline bool edges_equal(const HashEdge &edge_a, const HashEdge &edge_b)
    {
        return edge_a.facet_number != edge_b.facet_number && edge_a == edge_b;
    }

    // Connect edge_a with edge_b, update edge connection statistics.
    static void record_neighbors(stl_file *stl, const HashEdge &edge_a, const HashEdge &edge_b)
    {
        // Facet a's neighbor is facet b
        stl->neighbors_start[edge_a.facet_number].neighbor[edge_a.which_edge % STL_VERTICES_PER_FACET] = edge_b.facet_number;	/* sets the .neighbor part */
        stl->neighbors_start[edge_a.facet_number].which_vertex_not[edge_a.which_edge % STL_VERTICES_PER_FACET] = (edge_b.which_edge + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET; /* sets the .which_vertex_not part */

        // Facet b's neighbor is facet a
        stl->neighbors_start[edge_b.facet_number].neighbor[edge_b.which_edge % STL_VERTICES_PER_FACET] = edge_a.facet_number;	/* sets the .neighbor part */
        stl->neighbors_start[edge_b.facet_number].which_vertex_not[edge_b.which_edge % STL_VERTICES_PER_FACET] = (edge_a.which_edge + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET; /* sets the .which_vertex_not part */

        if ((edge_a.which_edge < STL_VERTICES_PER_FACET && edge_b.which_edge < STL_VERTICES_PER_FACET) || (edge_a.which_edge >= STL_VERTICES_PER_FACET && edge_b.which_edge >= STL_VERTICES_PER_FACET)) {
            // These facets are oriented in opposite directions, their normals are probably messed up.
            stl->neighbors_start[edge_a.facet_number].which_vertex_not[edge_a.which_edge % STL_VERTICES_PER_FACET] += STL_VERTICES_PER_FACET;
            stl->neighbors_start[edge_b.facet_number].which_vertex_not[edge_b.which_edge % STL_VERTICES_PER_FACET] += STL_VERTICES_PER_FACET;
        }

        // Count successful connects:
        // Total connects:
        stl->stats.connected_edges += 2;
        // Count individual connects:
        switch (stl->neighbors_start[edge_a.facet_number].num_neighbors()) {
        case kOneEdge:	++ stl->stats.connected_facets_1_edge; break;
        case kTwoEdges: ++ stl->stats.connected_facets_2_edge; break;
        case kThreeEdges: ++ stl->stats.connected_facets_3_edge; break;
        default: assert(false);
        }
        switch (stl->neighbors_start[edge_b.facet_number].num_neighbors()) {
        case kOneEdge:	++ stl->stats.connected_facets_1_edge; break;
        case kTwoEdges: ++ stl->stats.connected_facets_2_edge; break;
        case kThreeEdges: ++ stl->stats.connected_facets_3_edge; break;
        default: assert(false);
        }
    }

    // Walk the triangle fan around a vertex and snap all fan vertices to new_vertex.
    // vnot >= STL_VERTICES_PER_FACET means the neighboring edge is flipped relative to the current face.
    static void change_vertex_fan(stl_file *stl, int facet_num, int vnot, const stl_vertex &new_vertex)
    {
        int first_facet = facet_num;
        bool direction  = false;
        for (;;) {
            int pivot_vertex;
            int next_edge;
            if (vnot >= STL_VERTICES_PER_FACET) {
                if (direction) {
                    pivot_vertex = (vnot + 1) % STL_VERTICES_PER_FACET;
                    next_edge    = vnot % STL_VERTICES_PER_FACET;
                } else {
                    pivot_vertex = (vnot + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET;
                    next_edge    = pivot_vertex;
                }
                direction = !direction;
            } else {
                if (direction) {
                    pivot_vertex = (vnot + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET;
                    next_edge    = pivot_vertex;
                } else {
                    pivot_vertex = (vnot + 1) % STL_VERTICES_PER_FACET;
                    next_edge    = vnot;
                }
            }
            stl->facet_start[facet_num].vertex[pivot_vertex] = new_vertex;
            vnot      = stl->neighbors_start[facet_num].which_vertex_not[next_edge];
            facet_num = stl->neighbors_start[facet_num].neighbor[next_edge];
            if (facet_num == -1)
                break;
            if (facet_num == first_facet) {
                BOOST_LOG_TRIVIAL(info) << "Back to the first facet changing vertices: probably a mobius part. Try using a smaller tolerance or don't do a nearby check.";
                return;
            }
        }
    }

    static void match_neighbors_nearby(stl_file *stl, const HashEdge &edge_a, const HashEdge &edge_b)
    {
        record_neighbors(stl, edge_a, edge_b);

        // Which vertices to change
        int facet1 = -1;
        int facet2 = -1;
        int vertex1, vertex2;
        stl_vertex new_vertex1, new_vertex2;
        {
            int v1a; // pair 1, facet a
            int v1b; // pair 1, facet b
            int v2a; // pair 2, facet a
            int v2b; // pair 2, facet b
            // Find first pair.
            if (edge_a.which_edge < STL_VERTICES_PER_FACET) {
                v1a = edge_a.which_edge;
                v2a = (edge_a.which_edge + 1) % STL_VERTICES_PER_FACET;
            } else {
                v2a = edge_a.which_edge % STL_VERTICES_PER_FACET;
                v1a = (edge_a.which_edge + 1) % STL_VERTICES_PER_FACET;
            }
            if (edge_b.which_edge < STL_VERTICES_PER_FACET) {
                v1b = edge_b.which_edge;
                v2b = (edge_b.which_edge + 1) % STL_VERTICES_PER_FACET;
            } else {
                v2b = edge_b.which_edge % STL_VERTICES_PER_FACET;
                v1b = (edge_b.which_edge + 1) % STL_VERTICES_PER_FACET;
            }

            // Of the first pair, which vertex, if any, should be changed
            if (stl->facet_start[edge_a.facet_number].vertex[v1a] != stl->facet_start[edge_b.facet_number].vertex[v1b]) {
                // These facets are different.
                if (   (stl->neighbors_start[edge_a.facet_number].neighbor[v1a] == -1)
                    && (stl->neighbors_start[edge_a.facet_number].neighbor[(v1a + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET] == -1)) {
                    // This vertex has no neighbors.  This is a good one to change.
                    facet1 = edge_a.facet_number;
                    vertex1 = v1a;
                    new_vertex1 = stl->facet_start[edge_b.facet_number].vertex[v1b];
                } else {
                    facet1 = edge_b.facet_number;
                    vertex1 = v1b;
                    new_vertex1 = stl->facet_start[edge_a.facet_number].vertex[v1a];
                }
            }

            // Of the second pair, which vertex, if any, should be changed.
            if (stl->facet_start[edge_a.facet_number].vertex[v2a] == stl->facet_start[edge_b.facet_number].vertex[v2b]) {
                // These facets are different.
                if (  (stl->neighbors_start[edge_a.facet_number].neighbor[v2a] == -1)
                   && (stl->neighbors_start[edge_a.facet_number].neighbor[(v2a + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET] == -1)) {
                    // This vertex has no neighbors.  This is a good one to change.
                    facet2 = edge_a.facet_number;
                    vertex2 = v2a;
                    new_vertex2 = stl->facet_start[edge_b.facet_number].vertex[v2b];
                } else {
                    facet2 = edge_b.facet_number;
                    vertex2 = v2b;
                    new_vertex2 = stl->facet_start[edge_a.facet_number].vertex[v2a];
                }
            }
        }

        if (facet1 != -1) {
            int vnot1 = (facet1 == edge_a.facet_number) ? 
                (edge_a.which_edge + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET :
                (edge_b.which_edge + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET;
            if (((vnot1 + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET) == vertex1)
                vnot1 += STL_VERTICES_PER_FACET;
            change_vertex_fan(stl, facet1, vnot1, new_vertex1);
        }
        if (facet2 != -1) {
            int vnot2 = (facet2 == edge_a.facet_number) ?
                (edge_a.which_edge + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET :
                (edge_b.which_edge + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET;
            if (((vnot2 + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET) == vertex2)
                vnot2 += STL_VERTICES_PER_FACET;
            change_vertex_fan(stl, facet2, vnot2, new_vertex2);
        }
        stl->stats.edges_fixed += 2;
    }
};

// This function builds the neighbors list.  No modifications are made
// to any of the facets.  The edges are said to match only if all six
// floats of the first edge matches all six floats of the second edge.
void stl_check_facets_exact(stl_file *stl)
{
    assert(stl->facet_start.size() == stl->neighbors_start.size());

    stl->stats.connected_edges         = 0;
    stl->stats.connected_facets_1_edge = 0;
    stl->stats.connected_facets_2_edge = 0;
    stl->stats.connected_facets_3_edge = 0;

    // If any two of the three vertices are found to be exactally the same, call them degenerate and remove the facet.
    // Do it before the next step, as the next step stores references to the face indices in the hash tables and removing a facet
    // will break the references.
    for (uint32_t i = 0; i < stl->stats.number_of_facets;) {
        stl_facet &facet = stl->facet_start[i];
        if (facet.vertex[kV0] == facet.vertex[kV1] || facet.vertex[kV1] == facet.vertex[kV2] || facet.vertex[kV0] == facet.vertex[kV2]) {
            // Remove the degenerate facet.
            facet = stl->facet_start[-- stl->stats.number_of_facets];
            stl->facet_start.pop_back();
            stl->neighbors_start.pop_back();
            stl->stats.facets_removed += 1;
            stl->stats.degenerate_facets += 1;
        } else
            ++ i;
    }

    // Initialize hash table.
    HashTableEdges hash_table(stl->stats.number_of_facets);
    for (auto &neighbor : stl->neighbors_start)
        neighbor.reset();

    // Connect neighbor edges.
    for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
        const stl_facet &facet = stl->facet_start[i];
        for (int j = 0; j < STL_VERTICES_PER_FACET; ++ j) {
            HashEdge edge{};
            edge.facet_number = i;
            edge.which_edge = j;
            edge.load_exact(stl, &facet.vertex[j], &facet.vertex[(j + 1) % STL_VERTICES_PER_FACET]);
            hash_table.insert_edge_exact(stl, edge);
        }
    }

#if 0
    printf("Number of faces: %d, number of manifold edges: %d, number of connected edges: %d, number of unconnected edges: %d\r\n", 
        stl->stats.number_of_facets, stl->stats.number_of_facets * STL_VERTICES_PER_FACET, 
        stl->stats.connected_edges, stl->stats.number_of_facets * STL_VERTICES_PER_FACET - stl->stats.connected_edges);
#endif
}

void stl_check_facets_nearby(stl_file *stl, float tolerance)
{
    assert(stl->stats.connected_facets_3_edge <= stl->stats.connected_facets_2_edge);
    assert(stl->stats.connected_facets_2_edge <= stl->stats.connected_facets_1_edge);
    assert(stl->stats.connected_facets_1_edge <= stl->stats.number_of_facets);

    if (stl->stats.connected_facets_3_edge == stl->stats.number_of_facets)
        // No need to check any further.  All facets are connected.
        return;

    HashTableEdges hash_table(stl->stats.number_of_facets);
    for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
        const stl_facet &facet = stl->facet_start[i];
        for (int j = 0; j < STL_VERTICES_PER_FACET; j++) {
            if (stl->neighbors_start[i].neighbor[j] == -1) {
                HashEdge edge{};
                edge.facet_number = i;
                edge.which_edge = j;
                if (edge.load_nearby(stl, facet.vertex[j], facet.vertex[(j + 1) % STL_VERTICES_PER_FACET], tolerance))
                    // Only insert edges that have different keys.
                    hash_table.insert_edge_nearby(stl, edge);
            }
        }
    }
}

// Decrement connectivity statistics for one edge being removed from facet_num.
static void update_connects_remove_1(stl_file *stl, int facet_num)
{
    switch (stl->neighbors_start[facet_num].num_neighbors()) {
    case kZeroEdges: assert(false); break;
    case kOneEdge: -- stl->stats.connected_facets_1_edge; break;
    case kTwoEdges: -- stl->stats.connected_facets_2_edge; break;
    case kThreeEdges: -- stl->stats.connected_facets_3_edge; break;
    default: assert(false);
    }
}

// Remove facet at index facet_number, replacing it with the last facet.
static void remove_facet(stl_file *stl, int facet_number)
{
    ++ stl->stats.facets_removed;
    stl_neighbors &neighbors = stl->neighbors_start[facet_number];
    switch (neighbors.num_neighbors()) {
    case kThreeEdges: -- stl->stats.connected_facets_3_edge; // fall through
    case kTwoEdges: -- stl->stats.connected_facets_2_edge; // fall through
    case kOneEdge: -- stl->stats.connected_facets_1_edge; // fall through
    case kZeroEdges: break;
    default: assert(false);
    }
    if (facet_number < int(-- stl->stats.number_of_facets)) {
        // Removing a face that was not the last one: swap with last.
        stl->facet_start[facet_number] = stl->facet_start[stl->stats.number_of_facets];
        neighbors = stl->neighbors_start[stl->stats.number_of_facets];
        for (int i = 0; i < STL_VERTICES_PER_FACET; ++ i)
            if (neighbors.neighbor[i] != -1) {
                int &other_face_idx = stl->neighbors_start[neighbors.neighbor[i]].neighbor[(neighbors.which_vertex_not[i] + 1) % STL_VERTICES_PER_FACET];
                if (other_face_idx != stl->stats.number_of_facets) {
                    BOOST_LOG_TRIVIAL(info) << "in remove_facet: neighbor = " << other_face_idx << " numfacets = " << stl->stats.number_of_facets << " this is wrong";
                    return;
                }
                other_face_idx = facet_number;
            }
    }
    stl->facet_start.pop_back();
    stl->neighbors_start.pop_back();
}

// Collapse one degenerate (zero-area) facet, updating neighbor connectivity.
static void remove_degenerate(stl_file *stl, int facet)
{
    int edge_to_collapse = kV0;
    if (stl->facet_start[facet].vertex[kV0] == stl->facet_start[facet].vertex[kV1]) {
        if (stl->facet_start[facet].vertex[kV1] == stl->facet_start[facet].vertex[kV2]) {
            // All 3 vertices are equal. Collapse the edge with no neighbor if it exists.
            const int *nbr = stl->neighbors_start[facet].neighbor;
            edge_to_collapse = (nbr[kV0] == -1) ? kV0 : (nbr[kV1] == -1) ? kV1 : kV2;
        } else {
            edge_to_collapse = kV0;
        }
    } else if (stl->facet_start[facet].vertex[kV1] == stl->facet_start[facet].vertex[kV2]) {
        edge_to_collapse = kV1;
    } else if (stl->facet_start[facet].vertex[kV2] == stl->facet_start[facet].vertex[kV0]) {
        edge_to_collapse = kV2;
    } else {
        // No degenerate edge found; nothing to do.
        return;
    }
    int edge[STL_VERTICES_PER_FACET] = { (edge_to_collapse + 1) % STL_VERTICES_PER_FACET, (edge_to_collapse + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET, edge_to_collapse };
    int neighbor[] = {
        stl->neighbors_start[facet].neighbor[edge[kV0]],
        stl->neighbors_start[facet].neighbor[edge[kV1]],
        stl->neighbors_start[facet].neighbor[edge[kV2]]
    };
    int vnot[] = {
        stl->neighbors_start[facet].which_vertex_not[edge[kV0]],
        stl->neighbors_start[facet].which_vertex_not[edge[kV1]],
        stl->neighbors_start[facet].which_vertex_not[edge[kV2]]
    };
    if ((neighbor[kV0] == -1) && (neighbor[kV1] != -1))
        update_connects_remove_1(stl, neighbor[kV1]);
    if ((neighbor[kV1] == -1) && (neighbor[kV0] != -1))
        update_connects_remove_1(stl, neighbor[kV0]);
    if (neighbor[kV0] >= 0) {
        if (neighbor[kV1] >= 0) {
            // Adjust flip flags for which_vertex_not.
            if (vnot[kV0] >= STL_VERTICES_PER_FACET) {
                if (vnot[kV1] >= STL_VERTICES_PER_FACET) {
                    // Both neighbors flipped relative to removed face: orient correctly after removal.
                    vnot[kV0] -= STL_VERTICES_PER_FACET;
                    vnot[kV1] -= STL_VERTICES_PER_FACET;
                } else
                    // One neighbor flipped, one not: remaining neighbors will have inverted normals.
                    vnot[kV1] += STL_VERTICES_PER_FACET;
            } else if (vnot[kV1] >= STL_VERTICES_PER_FACET)
                // One neighbor flipped, one not: remaining neighbors will have inverted normals.
                vnot[kV0] += STL_VERTICES_PER_FACET;
        }
        stl->neighbors_start[neighbor[kV0]].neighbor[(vnot[kV0] + 1) % STL_VERTICES_PER_FACET] = (neighbor[kV0] == neighbor[kV1]) ? -1 : neighbor[kV1];
        stl->neighbors_start[neighbor[kV0]].which_vertex_not[(vnot[kV0] + 1) % STL_VERTICES_PER_FACET] = vnot[kV1];
    }
    if (neighbor[kV1] >= 0) {
        stl->neighbors_start[neighbor[kV1]].neighbor[(vnot[kV1] + 1) % STL_VERTICES_PER_FACET] = (neighbor[kV0] == neighbor[kV1]) ? -1 : neighbor[kV0];
        stl->neighbors_start[neighbor[kV1]].which_vertex_not[(vnot[kV1] + 1) % STL_VERTICES_PER_FACET] = vnot[kV0];
    }
    if (neighbor[kV2] >= 0) {
        update_connects_remove_1(stl, neighbor[kV2]);
        stl->neighbors_start[neighbor[kV2]].neighbor[(vnot[kV2] + 1) % STL_VERTICES_PER_FACET] = -1;
    }
    remove_facet(stl, facet);
}

void stl_remove_unconnected_facets(stl_file *stl)
{
    // A couple of things need to be done here.  One is to remove any completely unconnected facets (0 edges connected) since these are
    // useless and could be completely wrong.   The second thing that needs to be done is to remove any degenerate facets that were created during
    // stl_check_facets_nearby().

    // remove degenerate facets
    for (uint32_t i = 0; i < stl->stats.number_of_facets;)
        if (stl->facet_start[i].vertex[kV0] == stl->facet_start[i].vertex[kV1] ||
            stl->facet_start[i].vertex[kV0] == stl->facet_start[i].vertex[kV2] ||
            stl->facet_start[i].vertex[kV1] == stl->facet_start[i].vertex[kV2]) {
            remove_degenerate(stl, i);
//			assert(stl_validate(stl));
        } else
            ++ i;

    if (stl->stats.connected_facets_1_edge < static_cast<int>(stl->stats.number_of_facets)) {
        // There are some faces with no connected edge at all. Remove completely unconnected facets.
        for (uint32_t i = 0; i < stl->stats.number_of_facets;)
            if (stl->neighbors_start[i].num_neighbors() == 0) {
                // This facet is completely unconnected.  Remove it.
                remove_facet(stl, i);
                assert(stl_validate(stl));
            } else
                ++ i;
    }
}

void stl_fill_holes(stl_file *stl)
{
    // Insert all unconnected edges into hash list.
    HashTableEdges hash_table(stl->stats.number_of_facets);
    for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
        stl_facet facet = stl->facet_start[i];
        for (int j = 0; j < STL_VERTICES_PER_FACET; ++ j) {
            if(stl->neighbors_start[i].neighbor[j] != -1)
                continue;
            HashEdge edge{};
            edge.facet_number = i;
            edge.which_edge = j;
            edge.load_exact(stl, &facet.vertex[j], &facet.vertex[(j + 1) % STL_VERTICES_PER_FACET]);
            hash_table.insert_edge_exact(stl, edge);
        }
    }

    for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
        stl_facet facet = stl->facet_start[i];
        int neighbors_initial[STL_VERTICES_PER_FACET] = { stl->neighbors_start[i].neighbor[kV0], stl->neighbors_start[i].neighbor[kV1], stl->neighbors_start[i].neighbor[kV2] };
        int first_facet = i;
        for (int j = 0; j < STL_VERTICES_PER_FACET; ++ j) {
            if (stl->neighbors_start[i].neighbor[j] != -1)
                continue;

            stl_facet new_facet;
            new_facet.vertex[kV0] = facet.vertex[j];
            new_facet.vertex[kV1] = facet.vertex[(j + 1) % STL_VERTICES_PER_FACET];
            bool direction = neighbors_initial[(j + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET] == -1;
            int facet_num = i;
            int vnot = (j + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET;

            for (;;) {
                int pivot_vertex = 0;
                int next_edge = 0;
                if (vnot >= STL_VERTICES_PER_FACET) {
                    if (direction) {
                        pivot_vertex = (vnot + 1) % STL_VERTICES_PER_FACET;
                        next_edge = vnot % STL_VERTICES_PER_FACET;
                    } else {
                        pivot_vertex = (vnot + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET;
                        next_edge = pivot_vertex;
                    }
                    direction = ! direction;
                } else {
                    if(direction == 0) {
                        pivot_vertex = (vnot + 1) % STL_VERTICES_PER_FACET;
                        next_edge = vnot;
                    } else {
                        pivot_vertex = (vnot + STL_VERTICES_PER_FACET - 1) % STL_VERTICES_PER_FACET;
                        next_edge = pivot_vertex;
                    }
                }

                int next_facet = stl->neighbors_start[facet_num].neighbor[next_edge];
                if (next_facet == -1) {
                    new_facet.vertex[kV2] = stl->facet_start[facet_num].vertex[vnot % STL_VERTICES_PER_FACET];
                    stl_add_facet(stl, &new_facet);
                    for (int k = 0; k < STL_VERTICES_PER_FACET; ++ k) {
                        HashEdge edge{};
                        edge.facet_number = stl->stats.number_of_facets - 1;
                        edge.which_edge = k;
                        edge.load_exact(stl, &new_facet.vertex[k], &new_facet.vertex[(k + 1) % STL_VERTICES_PER_FACET]);
                        hash_table.insert_edge_exact(stl, edge);
                    }
                    break;
                }

                vnot = stl->neighbors_start[facet_num].which_vertex_not[next_edge];
                facet_num = next_facet;

                if (facet_num == first_facet) {
                    // back to the beginning
                    BOOST_LOG_TRIVIAL(info) << "Back to the first facet filling holes: probably a mobius part. Try using a smaller tolerance or don't do a nearby check.";
                    return;
                }
            }
        }
    }
}

void stl_add_facet(stl_file *stl, const stl_facet *new_facet)
{
    assert(stl->facet_start.size() == stl->stats.number_of_facets);
    assert(stl->neighbors_start.size() == stl->stats.number_of_facets);
    stl->facet_start.emplace_back(*new_facet);
    // note that the normal vector is not set here, just initialized to 0.
    stl->facet_start[stl->stats.number_of_facets].normal = stl_normal::Zero();
    stl->neighbors_start.emplace_back();
    ++ stl->stats.facets_added;
    ++ stl->stats.number_of_facets;
}
