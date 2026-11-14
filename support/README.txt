The six-picture 32x32 MPEG-2 fixture and portable decode scaffold are reused
from an earlier public scaffold at the identical upstream commit. ipb.m2v was generated
once with FFmpeg testsrc2 (rate=25, frames=6, mpeg2video, g=3, bf=1, q=5),
then a sequence-end code was appended. No FFmpeg or runtime download needed.
This fixture is frame-picture baseline material, not a field/dual-prime oracle.
All upstream source, license and tests remain intact.
