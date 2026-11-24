# zita-resampler

Zita-resampler is a C++ library for sample rate conversion of
audio signals. Full documentation is available in HTML format,
see the 'docs' directory.


Unreleased
----------

* Resampler and VResampler gain `setup_compact ()` overloads taking a
  `phase_budget` (32..1024): the coefficient table keeps at most that many
  equidistant phase intervals (the endpoint may take one extra row), so its
  memory no longer grows with the rational phase denominator.
* The fixed-ratio Resampler now accepts any positive sample rates in the
  unsigned int range with ratio >= 1/64. Ratios previously rejected only
  because the reduced output denominator exceeds 1000 (e.g. a measured
  48000 / 47999) use a compact table with a budget of 1024; previously
  supported ratios keep their exact, bit-identical tables.
* The exact rational clock is unchanged: rates are not rounded and the
  logical phase denominator is kept, with 64-bit quotient/remainder
  arithmetic so rates near UINT_MAX cannot overflow. `inpdist ()` always
  reflects the true input position, never the table index.
* Compact tables are shared only when the cut-off frequency, window length,
  physical layout and logical phase grids are compatible. With a budget of
  1024 and hlen 32 the maximum absolute error against a directly evaluated
  windowed-sinc reference is below 2e-4 for unit-bounded PCM (typically
  around 1e-7). See docs/resampler.html for the memory/accuracy trade-off.


Release 1.8.0  (30/12/2020)
---------------------------

* Added SSE2 support for Resampler and VResampler.
  This is enabled by default in the Makefile.
* Cleanup and some minor bug fixes.


Release 1.6.2  (25/08/2018)
---------------------------

* Maintenance release.


Release 1.6.0  (16/10/2016)
---------------------------

* Added VResampler::set_phase ().


Release 1.2.0  (25/09/2012)
---------------------------

* Added the zretune application and its manpage.


Release 1.1.0  (26/01/2012)
---------------------------

* VResampler class added - provides arbitrary and variable
  resampling ratio, see docs.

* This release is NOT binary compatible with previous ones
  (0.x.x) and requires recompilation of applications using it.

* This release is API compatible with the previous one. But if
  you are using the now deprecated filtlen() function please
  replace this by inpsize() which provides the same information. 

* The inpdist() function has been added, see docs.

* The ratio_a() and ratio_b() calls have been removed, if this
  is a problem (I'd be surprised) they can be added again.

* The include files are now in $PREFIX/include/zita-resampler/.
  Please DO remove any old ones manually after installing this
  version. Compiling using the old includes and linking with
  the new library will create havoc.

* #defines and static functions are added for compile time and
  run time version checking, see resampler-table.h. 

-- 
FA
