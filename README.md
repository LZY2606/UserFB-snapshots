# zita-resampler

Zita-resampler is a C++ library for sample rate conversion of
audio signals. Full documentation is available in HTML format,
see the 'docs' directory.

Per-channel source position offsets
-----------------------------------
`VResampler` and `CResampler` can fold a known inter-channel time
difference directly into the resampling instead of applying a separate
fractional delay first. Call `prepare_channel_alignment()` once after
`setup()` / `reset()`, then `set_channel_offsets()` with one offset per
channel, in input frames. A positive offset reads later than the nominal
source position. All channels still share one ratio, one fractional clock
and one set of counters; the offset is applied inside the real filter
sampling. See `docs/resampler.html` and `examples/channel_export.cc`
(built with `-DZITA_BUILD_EXAMPLES=ON`).


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
