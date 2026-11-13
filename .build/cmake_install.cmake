# Install script for directory: /Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/.build/libap4.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libap4.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libap4.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libap4.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/bento4" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Codecs/Ap4Ac3Parser.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Codecs/Ap4Ac4Parser.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Codecs/Ap4AdtsParser.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Codecs/Ap4AvcParser.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Codecs/Ap4BitStream.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Codecs/Ap4Eac3Parser.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Codecs/Ap4HevcParser.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Codecs/Ap4Mp4AudioInfo.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Codecs/Ap4NalParser.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap48bdlAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Ac4Utils.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4AinfAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Array.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Atom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4AtomFactory.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4AtomSampleTable.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Av1cAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4AvccAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4BlocAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4ByteStream.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Co64Atom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4ColrAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Command.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4CommandFactory.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4CommonEncryption.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Config.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Constants.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4ContainerAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4CttsAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Dac3Atom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Dac4Atom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4DataBuffer.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Debug.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Dec3Atom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4DecoderConfigDescriptor.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4DecoderSpecificInfoDescriptor.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Descriptor.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4DescriptorFactory.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4DmlpAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4DrefAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4DvccAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4DynamicCast.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4ElstAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4EsDescriptor.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4EsdsAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Expandable.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4File.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4FileByteStream.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4FileCopier.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4FileWriter.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4FragmentSampleTable.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4FrmaAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4FtypAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4GrpiAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4HdlrAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4HintTrackReader.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4HmhdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4HvccAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4IkmsAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Interfaces.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4IodsAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Ipmp.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4IproAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4IsfmAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4IsltAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4IsmaCryp.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4LinearReader.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4List.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Marlin.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4MdhdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4MehdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4MfhdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4MfroAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4MoovAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Movie.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4MovieFragment.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Mpeg2Ts.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4MvhdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4NmhdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4ObjectDescriptor.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4OdafAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4OddaAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4OdheAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4OhdrAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4OmaDcf.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4PdinAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Piff.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Processor.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Protection.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4PsshAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Results.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4RtpAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4RtpHint.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SLConfigDescriptor.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SaioAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SaizAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Sample.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SampleDescription.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SampleEntry.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SampleSource.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SampleTable.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SbgpAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SchmAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SdpAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SegmentBuilder.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SencAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SgpdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SidxAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SmhdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4StcoAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SthdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4String.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4StscAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4StsdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4StssAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4StszAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SttsAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Stz2Atom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4SyntheticSampleTable.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TencAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TfdtAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TfhdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TfraAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TimsAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TkhdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Track.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TrakAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TrefTypeAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TrexAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4TrunAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Tx3g.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Types.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4UrlAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Utils.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4UuidAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4Version.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4VmhdAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Core/Ap4VpccAtom.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Crypto/Ap4AesBlockCipher.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Crypto/Ap4Hmac.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Crypto/Ap4KeyWrap.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/Crypto/Ap4StreamCipher.h"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/Source/C++/MetaData/Ap4MetaData.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/.build/generated/bento4Config.cmake"
    "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/.build/generated/bento4ConfigVersion.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/.build/generated/bento4.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets.cmake"
         "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/.build/CMakeFiles/Export/e0dd49b2eb3ca1303803848168356d30/bento4Targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/.build/CMakeFiles/Export/e0dd49b2eb3ca1303803848168356d30/bento4Targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/.build/CMakeFiles/Export/e0dd49b2eb3ca1303803848168356d30/bento4Targets-release.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/.build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Y1102/attempts/20261113T180534852568Z/repo/.build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
