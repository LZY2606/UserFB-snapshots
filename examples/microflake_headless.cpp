//-----------------------------------------------------------------------------
// Headless SGGX microflake example.
//
// Renders the original sphere scene from the original camera with a sparse
// microflake slab occupying the middle of the room, using two tensor
// orientations (axis-aligned and rotated). Writes axis.ppm and rotated.ppm
// into the directory given as the first argument and prints the linear mean,
// the volume collision count and the number of cell visits for each image.
// Both images are produced by the original Radiance production chain: the
// Radiance overload in volume/radiance_medium.hpp (which shares the original
 the surface branch), the original sphere scene and WritePPM.
//
// Usage: microflake_example OUTDIR
//-----------------------------------------------------------------------------
