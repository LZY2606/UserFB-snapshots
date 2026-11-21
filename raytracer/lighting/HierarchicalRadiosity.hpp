#pragma once

/**
   Hierarchical radiosity for a small set of diffuse triangles.

   The participating triangles are subdivided, for lighting only, into
   4^depth equal-area leaf triangles per original face. A quadtree per
   original face drives hierarchical link refinement: node pairs are linked
   directly when the geometry kernel is nearly constant over their descendant
   leaf pairs and no other geometry can occlude the bundle, otherwise the
   larger splittable end is refined. Leaf-leaf pairs that cannot be linked
   fall back to an exact open-segment visibility query through the World.

   All transport arithmetic is double precision. The source scene must stay
   static and alive for the lifetime of the object: World and the registered
   Triangle pointers are held by reference/pointer and never copied.
*/

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "../utilities/Point3D.hpp"
#include "../utilities/RGBColor.hpp"

class Triangle;
class World;

// One hierarchical link between two quadtree nodes. A link is shared by both
// transport directions; primitive data is never duplicated per direction.
struct RadiosityLink {
    int node_a;          // global index of the first node.
    int node_b;          // global index of the second node.
    double kernel;       // kernel evaluated at the two node centroids.
    double kernel_lo;    // conservative lower bound over descendant leaf pairs.
    double kernel_hi;    // conservative upper bound over descendant leaf pairs.
    bool exact;          // leaf-to-leaf link from a real visibility query.
};

class HierarchicalRadiosity {
public:
    // Builds the quadtrees, refines all links and solves the radiosity system.
    // tris must contain 1..32 distinct triangles already added to world.
    // rho channels must be in [0, 0.8], emission channels in [0, 4],
    // depth in [0, 4] and eta in [1e-4, 0.1]. On failure ok() is false and
    // error() explains why.
    HierarchicalRadiosity(World &world,
                          const std::vector<Triangle *> &tris,
                          const std::vector<RGBColor> &reflectance,
                          const std::vector<RGBColor> &emission,
                          int depth, double eta);

    ~HierarchicalRadiosity();

    bool ok() const;
    const std::string &error() const;

    // Tree sizes.
    std::size_t node_count() const;
    std::size_t leaf_count() const;
    std::size_t original_count() const;

    // Node introspection for link/kernel diagnostics.
    int node_level(int node) const;
    bool node_is_leaf(int node) const;
    int node_original(int node) const;
    Point3D node_centroid(int node) const;
    // Global leaf indices below the given node.
    std::vector<std::size_t> descendant_leaves(int node) const;
    // Quadtree node holding the given global leaf.
    int leaf_node(std::size_t leaf) const;

    // Leaf accessors. Leaves are numbered globally across all originals.
    double leaf_area(std::size_t leaf) const;
    int leaf_original(std::size_t leaf) const;
    const Triangle *leaf_triangle(std::size_t leaf) const;
    void leaf_bary_domain(std::size_t leaf,
                          double &beta_lo, double &beta_hi,
                          double &gamma_lo, double &gamma_hi) const;
    // Barycentric weights of the leaf centroid inside the original triangle.
    void leaf_centroid_bary(std::size_t leaf, double &beta, double &gamma) const;
    Point3D leaf_centroid(std::size_t leaf) const;
    void leaf_radiosity(std::size_t leaf,
                        double &r, double &g, double &b) const;
    void leaf_emission(std::size_t leaf,
                       double &r, double &g, double &b) const;
    void leaf_reflectance(std::size_t leaf,
                          double &r, double &g, double &b) const;

    // Maps a Whitted hit on a registered triangle (barycentric weights of the
    // triangle's v1 and v2) to the owning lighting leaf. Shared sub-face
    // boundaries belong to the deterministically smaller child index.
    int leaf_for_hit(const Triangle *tri, double beta, double gamma) const;

    // Links and diagnostics.
    const std::vector<RadiosityLink> &links() const;
    std::size_t full_leaf_pair_count() const; // dense cross-tri leaf pairs.
    unsigned long long visibility_queries() const; // exact segment queries.
    std::size_t storage_peak_bytes() const;
    unsigned long long operator_applications() const;
    unsigned long long node_tests() const;
    unsigned long long primitive_tests() const;
    // max_i sum_j A_j K_ij measured on the compressed operator (B = 1).
    double compressed_row_sum_max() const;

    // Re-solve with a positive iteration budget, reusing tree and links and
    // issuing no visibility queries. On budget exhaustion the previous full
    // solution is retained. Returns false when the budget is zero or exhausted.
    bool solve(unsigned max_iterations);

    // Per-original-face emission update [0, 4] per channel. Reuses the tree
    // and links and performs no occlusion queries. Invalid values or a failed
    // solve leave the previous complete solution untouched.
    bool set_emission(int original_index, const RGBColor &emission);

private:
    struct Node;
    struct Work;

    World &world;
    std::vector<Triangle *> tris;
    std::vector<Node> nodes;
    std::vector<int> roots;              // root node index per original.
    std::vector<int> leaf_start;         // first global leaf per original.
    std::vector<std::array<double, 3>> rho;
    std::vector<std::array<double, 3>> emission;
    std::vector<std::array<double, 3>> radiosity; // indexed by global leaf.
    std::vector<RadiosityLink> link_store;
    int depth;
    double eta;
    bool built;
    std::string failure;
    unsigned long long vis_queries;
    std::size_t peak_bytes;
    unsigned long long op_count;
    unsigned long long saved_node_tests;
    unsigned long long saved_prim_tests;
    double row_sum_max;
    std::size_t working_bytes; // temporaries live during the current phase.

    bool validate_inputs(const std::vector<RGBColor> &reflectance,
                         const std::vector<RGBColor> &emission);
    void build_trees();
    void refine_links();
    bool kernel_interval(const Node &a, const Node &b,
                         double &lo, double &hi) const;
    double centroid_kernel(const Node &a, const Node &b) const;
    bool visible(const Node &a, const Node &b);
    void account_storage(std::size_t stack_size);
    bool apply_operator(const std::vector<std::array<double, 3>> &B,
                        std::vector<std::array<double, 3>> &I);
    bool run_solver(unsigned max_iterations);
};
