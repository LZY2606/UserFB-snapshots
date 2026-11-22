/***************************************************************************
    copyright            : (C) 2002 - 2008 by Scott Wheeler
    email                : wheeler@kde.org
 ***************************************************************************/

/***************************************************************************
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Lesser General Public License version   *
 *   2.1 as published by the Free Software Foundation.                     *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful, but   *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with this library; if not, write to the Free Software   *
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA         *
 *   02110-1301  USA                                                       *
 *                                                                         *
 *   Alternatively, this file is available under the Mozilla Public        *
 *   License Version 1.1.  You may obtain a copy of the License at         *
 *   http://www.mozilla.org/MPL/                                           *
 ***************************************************************************/

#include "taglib_export.h"
#include "tfile.h"
#include "tbytevectorlist.h"

#ifndef TAGLIB_OGGFILE_H
#define TAGLIB_OGGFILE_H

namespace TagLib {

  //! A namespace for the classes used by Ogg-based metadata files

  namespace Ogg {

    class PageHeader;

    //! An implementation of TagLib::File with some helpers for Ogg based formats

    /*!
     * This is an implementation of Ogg file page and packet rendering and is of
     * use to Ogg based formats.  While the API is small this handles the
     * non-trivial details of breaking up an Ogg stream into packets and makes
     * these available (via subclassing) to the codec meta data implementations.
     */

    class TAGLIB_EXPORT File : public TagLib::File
    {
    public:
      virtual ~File();

      /*!
       * Returns the packet contents for the i-th packet (starting from zero)
       * in the Ogg bitstream.
       *
       * \warning This requires reading at least the packet header for every page
       * up to the requested page.
       */
      ByteVector packet(unsigned int i);

      /*!
       * Sets the packet with index \a i to the value \a p.
       */
      void setPacket(unsigned int i, const ByteVector &p);

      /*!
       * Returns a pointer to the PageHeader for the first page in the stream or
       * null if the page could not be found.
       */
      const PageHeader *firstPageHeader();

      /*!
       * Returns a pointer to the PageHeader for the last page in the stream or
       * null if the page could not be found.
       */
      const PageHeader *lastPageHeader();

      virtual bool save();

      /*!
       * Returns the total number of bytes occupied by the pages of the
       * selected logical stream.  For a plain (non multiplexed) file this
       * is simply the length of the file.
       */
      long long streamPageBytes();

    protected:
      /*!
       * Constructs an Ogg file from \a file.
       *
       * \note This constructor is protected since Ogg::File shouldn't be
       * instantiated directly but rather should be used through the codec
       * specific subclasses.
       */
      File(FileName file);

      /*!
       * Constructs an Ogg file from \a stream.
       *
       * \note This constructor is protected since Ogg::File shouldn't be
       * instantiated directly but should be used through the codec
       * specific subclasses.
       *
       * \note TagLib will *not* take ownership of the stream, the caller is
       * responsible for deleting it after the File object.
       */
      File(IOStream *stream);

      /*!
       * Constructs an Ogg file from \a file and selects the logical Ogg
       * stream with the serial number \a serialNumber in a multiplexed
       * (chained) file.  Every page, packet and audio property accessed
       * through this instance is then restricted to that logical stream.
       *
       * \note This constructor is protected since Ogg::File shouldn't be
       * instantiated directly but rather should be used through the codec
       * specific subclasses.
       */
      File(FileName file, unsigned int serialNumber);

      /*!
       * Constructs an Ogg file from \a stream and selects the logical Ogg
       * stream with the serial number \a serialNumber in a multiplexed
       * (chained) file.  Every page, packet and audio property accessed
       * through this instance is then restricted to that logical stream.
       *
       * \note TagLib will *not* take ownership of the stream, the caller is
       * responsible for deleting it after the File object.
       */
      File(IOStream *stream, unsigned int serialNumber);

      /*!
       * Returns true if this instance works on a single logical stream
       * selected by an explicit serial number (the four argument
       * constructors).
       */
      bool multiplexed() const;

      /*!
       * Returns the serial number of the selected logical stream when the
       * file was opened with an explicit serial number, and zero otherwise.
       */
      unsigned int selectedSerialNumber() const;

      /*!
       * Scans every physical page in the file and builds the index of the
       * selected logical stream.  Returns true if a beginning-of-stream
       * page with the selected serial number was found.  Only meaningful for
       * multiplexed instances.
       */
      bool indexLogicalStream();

      /*!
       * Checks the structural assumptions about the selected logical stream
       * (continuous page numbering, consistent continuation flags, an end
       * of stream page, no dangling packet and a page boundary between the
       * three header packets and the first audio packet).  Returns true if
       * the stream may be rewritten.  The file is not touched when this
       * returns false.
       */
      bool validateLogicalStream();

      /*!
       * Discards the page index so that it is rebuilt from the rewritten
       * file on the next access.
       */
      void invalidatePageIndex();

    private:
      File(const File &);
      File &operator=(const File &);

      /*!
       * Reads the pages from the beginning of the file until enough to compose
       * the requested packet.
       */
      bool readPages(unsigned int i);

      /*!
       * Writes the requested packet to the file.
       */
      void writePacket(unsigned int i, const ByteVector &packet);

      //! Logical page index and multiplexed rewrite support.
      struct PhysicalPage;

      bool scanPhysicalPages();
      ByteVector readLogicalPacket(unsigned int i);
      bool writeLogicalPacket(unsigned int i, const ByteVector &packet);
      void copyRange(long source, long long length, long destination,
                     unsigned long blockSize);

      class FilePrivate;
      FilePrivate *d;
    };

  }  // namespace Ogg
}  // namespace TagLib

#endif
