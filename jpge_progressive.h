// jpge_progressive.h - internal helpers for jpge's progressive (SOF2) encoder.
// Not part of the public API: include this only from jpge.cpp / jpeg_progressive.cpp.
#ifndef JPEG_PROGRESSIVE_H
#define JPEG_PROGRESSIVE_H

namespace jpge
{
	namespace prog
	{
		// One entry of the progressive scan script.
		struct scan_desc
		{
			int num_comps;
			int comps[3];
			int ss, se;   // spectral selection
			int ah, al;   // successive approximation
		};

		// Maximum scans in the fixed script (1 DC first + N AC first + 1 DC refine + N AC refine).
		enum { MAX_SCANS = 10 };

		// Scans are emitted in this fixed order:
		//   1 x interleaved DC first      (Ss=0, Se=0,  Ah=0, Al=1)
		//   N x per-component AC first    (Ss=1, Se=63, Ah=0, Al=1)
		//   1 x interleaved DC refinement (Ss=0, Se=0,  Ah=1, Al=0)
		//   N x per-component AC refine   (Ss=1, Se=63, Ah=1, Al=0)
		// Returns the number of scans written to out[].
		int make_scan_script(int num_components, scan_desc* out);
	} // namespace prog
} // namespace jpge

#endif // JPEG_PROGRESSIVE_H
