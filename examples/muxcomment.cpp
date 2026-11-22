/* Copyright (C) 2026 Scott Wheeler <wheeler@kde.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// This example shows how to edit the Vorbis comment of one logical stream in
// a multiplexed Ogg file by selecting the stream with its serial number.
// Both four argument Vorbis::File constructors are shown: one taking a file
// name and one taking an IOStream.

#include <cstdlib>
#include <cstring>
#include <iostream>

#include <tbytevectorstream.h>
#include <vorbisfile.h>

using namespace std;

namespace
{
  unsigned int parseSerial(const char *text)
  {
    return static_cast<unsigned int>(strtoul(text, 0, 0));
  }
}

int main(int argc, char *argv[])
{
  if(argc != 4) {
    cerr << "Usage: " << argv[0]
         << " <file.ogg> <serial-number> <artist>" << endl
         << "The serial number accepts decimal (1234) or hexadecimal"
         << " (0x1234) notation." << endl;
    return 1;
  }

  const char *fileName = argv[1];
  const unsigned int serial = parseSerial(argv[2]);

  // File name entry.  Tags, packets, page headers, audio properties and
  // save() are all restricted to the logical stream with this serial.
  {
    TagLib::Vorbis::File file(fileName, true,
                              TagLib::Vorbis::Properties::Average, serial);
    if(!file.isValid()) {
      cerr << "No Vorbis stream with serial " << serial
           << " was found in " << fileName << endl;
      return 1;
    }

    cout << "Current artist: "
         << file.tag()->artist().toCString(true) << endl;
    file.tag()->setArtist(argv[3]);
    if(!file.save()) {
      cerr << "Saving " << fileName << " failed." << endl;
      return 1;
    }
  }

  // IOStream entry, reopening the rewritten file to confirm the change.
  {
    TagLib::ByteVector bytes;
    if(FILE *input = fopen(fileName, "rb")) {
      fseek(input, 0, SEEK_END);
      long size = ftell(input);
      fseek(input, 0, SEEK_SET);
      bytes.resize(static_cast<unsigned int>(size));
      if(fread(bytes.data(), 1, size, input) !=
         static_cast<size_t>(size)) {
        cerr << "Could not read " << fileName << endl;
        return 1;
      }
      fclose(input);
    }
    else {
      cerr << "Could not open " << fileName << endl;
      return 1;
    }

    TagLib::ByteVectorStream stream(bytes);
    TagLib::Vorbis::File file(&stream, true,
                              TagLib::Vorbis::Properties::Average, serial);
    if(!file.isValid()) {
      cerr << "The rewritten file is no longer valid." << endl;
      return 1;
    }
    cout << "Saved artist: "
         << file.tag()->artist().toCString(true) << endl;
    if(file.audioProperties())
      cout << "Length: "
           << file.audioProperties()->lengthInMilliseconds() << " ms"
           << endl;
  }

  return 0;
}
