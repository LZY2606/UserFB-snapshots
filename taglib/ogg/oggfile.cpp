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

#include <algorithm>

#include <tbytevectorlist.h>
#include <tmap.h>
#include <tstring.h>
#include <tdebug.h>

#include "oggfile.h"
#include "oggpage.h"
#include "oggpageheader.h"

using namespace TagLib;

namespace
{
  // Returns the first packet index of the right next page to the given one.
  unsigned int nextPacketIndex(const Ogg::Page *page)
  {
    if(page->header()->lastPacketCompleted())
      return page->firstPacketIndex() + page->packetCount();
    return page->firstPacketIndex() + page->packetCount() - 1;
  }

  // Size of the blocks used when file contents are moved around.  No page or
  // packet payload is ever cached in full, only fixed size blocks like this.
  const unsigned long MoveBlockSize = 8192;
}  // namespace

// Physical page descriptor.  Only metadata is stored, never the page body,
// so indexing a multiplexed file does not pull the whole file into memory.
struct Ogg::File::PhysicalPage
{
  PhysicalPage() :
    offset(0),
    size(0),
    serial(0),
    sequence(0),
    granule(0),
    continued(false),
    bos(false),
    eos(false),
    lastCompleted(false),
    logical(false),
    firstPacketIndex(-1) {}

  long offset;
  int size;
  unsigned int serial;
  unsigned int sequence;
  long long granule;
  bool continued;
  bool bos;
  bool eos;
  bool lastCompleted;
  List<int> packetSizes;
  bool logical;

  // Index of the first packet (partial or whole) appearing on this page,
  // counted inside the selected logical stream.
  int firstPacketIndex;
};

class Ogg::File::FilePrivate
{
public:
  FilePrivate() :
    multiplex(false),
    serialNumber(0),
    indexed(false),
    indexValid(false),
    firstPageHeader(0),
    lastPageHeader(0)
  {
    pages.setAutoDelete(true);
    physicalPages.setAutoDelete(true);
  }

  ~FilePrivate()
  {
    delete firstPageHeader;
    delete lastPageHeader;
  }

  bool multiplex;
  unsigned int serialNumber;
  bool indexed;
  bool indexValid;

  // Physical order of every page in the file, built for multiplexed files.
  List<PhysicalPage *> physicalPages;

  // Physical page indices of the pages of the selected logical stream, in
  // logical page order.
  List<unsigned int> logicalPages;

  unsigned int streamSerialNumber;
  List<Page *> pages;
  PageHeader *firstPageHeader;
  PageHeader *lastPageHeader;
  Map<unsigned int, ByteVector> dirtyPackets;
};

////////////////////////////////////////////////////////////////////////////////
// public members
////////////////////////////////////////////////////////////////////////////////

Ogg::File::~File()
{
  delete d;
}

ByteVector Ogg::File::packet(unsigned int i)
{
  if(d->multiplex) {
    if(d->dirtyPackets.contains(i))
      return d->dirtyPackets[i];

    if(!indexLogicalStream()) {
      debug("Ogg::File::packet() -- Could not find the requested logical stream.");
      return ByteVector();
    }

    return readLogicalPacket(i);
  }

  // Check to see if we're called setPacket() for this packet since the last
  // save:

  if(d->dirtyPackets.contains(i))
    return d->dirtyPackets[i];

  // If we haven't indexed the page where the packet we're interested in starts,
  // begin reading pages until we have.

  if(!readPages(i)) {
    debug("Ogg::File::packet() -- Could not find the requested packet.");
    return ByteVector();
  }

  // Look for the first page in which the requested packet starts.

  List<Page *>::ConstIterator it = d->pages.begin();
  while((*it)->containsPacket(i) == Page::DoesNotContainPacket)
    ++it;

  // If the packet is completely contained in the first page that it's in.

  // If the packet is *not* completely contained in the first page that it's a
  // part of then that packet trails off the end of the page.  Continue appending
  // the pages' packet data until we hit a page that either does not end with the
  // packet that we're fetching or where the last packet is complete.

  ByteVector packet = (*it)->packets()[i - (*it)->firstPacketIndex()];

  while(nextPacketIndex(*it) <= i) {
    ++it;
    packet.append((*it)->packets().front());
  }

  return packet;
}

void Ogg::File::setPacket(unsigned int i, const ByteVector &p)
{
  if(d->multiplex) {
    // The actual structure check happens before the first write in save().
    d->dirtyPackets[i] = p;
    return;
  }

  if(!readPages(i)) {
    debug("Ogg::File::setPacket() -- Could not set the requested packet.");
    return;
  }

  d->dirtyPackets[i] = p;
}

const Ogg::PageHeader *Ogg::File::firstPageHeader()
{
  if(d->multiplex) {
    if(!indexLogicalStream())
      return 0;

    if(!d->firstPageHeader) {
      const PhysicalPage *page = d->physicalPages[d->logicalPages.front()];
      d->firstPageHeader = new PageHeader(this, page->offset);
    }

    return d->firstPageHeader->isValid() ? d->firstPageHeader : 0;
  }

  if(!d->firstPageHeader) {
    const long firstPageHeaderOffset = find("OggS");
    if(firstPageHeaderOffset < 0)
      return 0;

    d->firstPageHeader = new PageHeader(this, firstPageHeaderOffset);
  }

  return d->firstPageHeader->isValid() ? d->firstPageHeader : 0;
}

const Ogg::PageHeader *Ogg::File::lastPageHeader()
{
  if(d->multiplex) {
    if(!indexLogicalStream())
      return 0;

    if(!d->lastPageHeader) {
      const PhysicalPage *page = d->physicalPages[d->logicalPages.back()];
      d->lastPageHeader = new PageHeader(this, page->offset);
    }

    return d->lastPageHeader->isValid() ? d->lastPageHeader : 0;
  }

  if(!d->lastPageHeader) {
    const long lastPageHeaderOffset = rfind("OggS");
    if(lastPageHeaderOffset < 0)
      return 0;

    d->lastPageHeader = new PageHeader(this, lastPageHeaderOffset);
  }

  return d->lastPageHeader->isValid() ? d->lastPageHeader : 0;
}

bool Ogg::File::save()
{
  if(readOnly()) {
    debug("Ogg::File::save() - Cannot save to a read only file.");
    return false;
  }

  if(d->multiplex) {
    // Every structural assumption is checked before the first byte is
    // written, so a failed save leaves the file untouched.
    if(!indexLogicalStream() || !validateLogicalStream())
      return false;

    Map<unsigned int, ByteVector>::ConstIterator it;
    for(it = d->dirtyPackets.begin(); it != d->dirtyPackets.end(); ++it) {
      if(!writeLogicalPacket(it->first, it->second))
        return false;
    }

    d->dirtyPackets.clear();
    return true;
  }

  Map<unsigned int, ByteVector>::ConstIterator it;
  for(it = d->dirtyPackets.begin(); it != d->dirtyPackets.end(); ++it)
    writePacket(it->first, it->second);

  d->dirtyPackets.clear();

  return true;
}

////////////////////////////////////////////////////////////////////////////////
// protected members
////////////////////////////////////////////////////////////////////////////////

Ogg::File::File(FileName file) :
  TagLib::File(file),
  d(new FilePrivate())
{
}

Ogg::File::File(IOStream *stream) :
  TagLib::File(stream),
  d(new FilePrivate())
{
}

Ogg::File::File(FileName file, unsigned int serialNumber) :
  TagLib::File(file),
  d(new FilePrivate())
{
  d->multiplex = true;
  d->serialNumber = serialNumber;
}

Ogg::File::File(IOStream *stream, unsigned int serialNumber) :
  TagLib::File(stream),
  d(new FilePrivate())
{
  d->multiplex = true;
  d->serialNumber = serialNumber;
}

bool Ogg::File::multiplexed() const
{
  return d->multiplex;
}

unsigned int Ogg::File::selectedSerialNumber() const
{
  return d->multiplex ? d->serialNumber : 0;
}

bool Ogg::File::indexLogicalStream()
{
  if(d->indexed)
    return d->indexValid;

  d->indexed = true;
  d->indexValid = false;

  if(!scanPhysicalPages())
    return false;

  // Mark the pages of the selected logical stream and make sure its
  // beginning of stream page occurs exactly once.

  bool bosFound = false;
  for(List<PhysicalPage *>::ConstIterator it = d->physicalPages.begin();
      it != d->physicalPages.end(); ++it) {
    PhysicalPage *page = *it;
    page->logical = (page->serial == d->serialNumber);

    if(page->logical && page->bos) {
      if(bosFound)
        return false;
      bosFound = true;
    }
  }

  if(!bosFound)
    return false;

  // Collect the selected pages in physical (and therefore logical) order.

  for(unsigned int i = 0; i < d->physicalPages.size(); ++i) {
    if(d->physicalPages[i]->logical)
      d->logicalPages.append(i);
  }

  // Assign logical packet indices to every selected page.  PageHeader's
  // packetSizes() lists every packet touched by the page, including a final
  // partial entry for a packet left open when lastPacketCompleted() is false.
  // A page therefore terminates (entries - 1) packets when it is open ended,
  // and all entries otherwise.

  int nextPacket = 0;   // index of the next packet that will start
  int openPacket = -1;  // index of the packet currently spanning pages
  for(List<unsigned int>::ConstIterator it = d->logicalPages.begin();
      it != d->logicalPages.end(); ++it) {
    PhysicalPage *page = d->physicalPages[*it];
    const int entries = static_cast<int>(page->packetSizes.size());
    const bool openEnded = !page->lastCompleted;
    const int terminating = entries - (openEnded ? 1 : 0);

    if(page->continued)
      page->firstPacketIndex = openPacket;
    else
      page->firstPacketIndex = nextPacket;

    if(openEnded) {
      // The terminating packets come first, then one newly (or still) open
      // packet.
      openPacket = page->firstPacketIndex + terminating;
      nextPacket = openPacket + 1;
    }
    else {
      nextPacket = page->firstPacketIndex + entries;
      openPacket = -1;
    }
  }

  d->indexValid = true;
  return true;
}

bool Ogg::File::validateLogicalStream()
{
  if(!d->indexValid || d->logicalPages.isEmpty())
    return false;

  // All beginning of stream pages must precede every other page.

  bool nonBosSeen = false;
  for(List<PhysicalPage *>::ConstIterator it = d->physicalPages.begin();
      it != d->physicalPages.end(); ++it) {
    const PhysicalPage *page = *it;
    if(page->bos) {
      if(nonBosSeen)
        return false;
    }
    else
      nonBosSeen = true;
  }

  // The selected stream needs the three header packets and at least one
  // audio packet.

  const PhysicalPage *first = d->physicalPages[d->logicalPages.front()];
  const PhysicalPage *last  = d->physicalPages[d->logicalPages.back()];

  if(!first->bos || first->sequence != 0 || first->continued ||
     !first->lastCompleted || first->packetSizes.size() != 1)
    return false;

  // Walk the selected pages checking numbering, continuation flags, the end
  // of stream page and the header granule positions.

  bool openPacket = false;
  bool eosSeen = false;
  unsigned int expectedSequence = 0;
  const PhysicalPage *setupPage = 0;

  for(List<unsigned int>::ConstIterator it = d->logicalPages.begin();
      it != d->logicalPages.end(); ++it) {
    const PhysicalPage *page = d->physicalPages[*it];

    if(eosSeen)
      return false;
    if(page->sequence != expectedSequence)
      return false;
    ++expectedSequence;

    // A continued page must continue a packet left open by the previous
    // page, and an open packet must be continued by the next page.
    if(page->continued != openPacket)
      return false;
    openPacket = !page->lastCompleted;

    if(page->eos)
      eosSeen = true;

    // Pages touching only the three header packets (identified by their first
    // packet index) follow the granule rule: zero whenever a header packet
    // finishes on the page, minus one when the page only carries an unfinished
    // packet.  The final packetSizes() entry of an open ended page is the
    // partial packet, so it does not count as terminating here.
    if(page->firstPacketIndex <= 2) {
      const int terminating = static_cast<int>(page->packetSizes.size())
                            - (page->lastCompleted ? 0 : 1);
      if(terminating > 0 ? page->granule != 0 : page->granule != -1)
        return false;
    }

    // Remember the page on which the setup header (packet 2) completes; it
    // has to end exactly at a page boundary.
    const int lastFinishedIndex = page->firstPacketIndex
                                + static_cast<int>(page->packetSizes.size()) - 1;
    if(page->lastCompleted && lastFinishedIndex == 2)
      setupPage = page;
  }

  // The stream has to end with its end of stream page, with no packet left
  // dangling.
  if(!eosSeen || !last->eos || openPacket)
    return false;

  // The three header packets and the first audio packet need at least four
  // packets, and the setup header has to finish exactly at the end of a page,
  // with audio starting on the next selected page.
  if(!setupPage || last->firstPacketIndex < 3)
    return false;

  List<unsigned int>::ConstIterator setupIt = d->logicalPages.begin();
  for(; setupIt != d->logicalPages.end(); ++setupIt) {
    if(d->physicalPages[*setupIt] == setupPage)
      break;
  }
  if(setupIt == d->logicalPages.end())
    return false;

  ++setupIt;
  if(setupIt == d->logicalPages.end())
    return false;

  const PhysicalPage *firstAudioPage = d->physicalPages[*setupIt];
  if(firstAudioPage->continued || firstAudioPage->firstPacketIndex != 3)
    return false;

  return true;
}

long long Ogg::File::streamPageBytes()
{
  if(!d->multiplex)
    return length();

  long long total = 0;
  for(List<unsigned int>::ConstIterator it = d->logicalPages.begin();
      it != d->logicalPages.end(); ++it)
    total += d->physicalPages[*it]->size;

  return total;
}

void Ogg::File::invalidatePageIndex()
{
  d->physicalPages.clear();
  d->logicalPages.clear();
  d->pages.clear();
  delete d->firstPageHeader;
  d->firstPageHeader = 0;
  delete d->lastPageHeader;
  d->lastPageHeader = 0;
  d->indexed = false;
  d->indexValid = false;
}

////////////////////////////////////////////////////////////////////////////////
// private members
////////////////////////////////////////////////////////////////////////////////

bool Ogg::File::readPages(unsigned int i)
{
  while(true) {
    unsigned int packetIndex;
    long offset;

    if(d->pages.isEmpty()) {
      packetIndex = 0;
      offset = find("OggS");
      if(offset < 0)
        return false;
    }
    else {
      const Page *page = d->pages.back();
      packetIndex = nextPacketIndex(page);
      offset = page->fileOffset() + page->size();
    }

    // Enough pages have been fetched.

    if(packetIndex > i)
      return true;

    // Read the next page and add it to the page list.

    Page *nextPage = new Page(this, offset);
    if(!nextPage->header()->isValid()) {
      delete nextPage;
      return false;
    }

    nextPage->setFirstPacketIndex(packetIndex);
    d->pages.append(nextPage);

    if(nextPage->header()->lastPageOfStream())
      return false;
  }
}

void Ogg::File::writePacket(unsigned int i, const ByteVector &packet)
{
  if(!readPages(i)) {
    debug("Ogg::File::writePacket() -- Could not find the requested packet.");
    return;
  }

  // Look for the pages where the requested packet should belong to.

  List<Page *>::ConstIterator it = d->pages.begin();
  while((*it)->containsPacket(i) == Page::DoesNotContainPacket)
    ++it;

  const Page *firstPage = *it;

  while(nextPacketIndex(*it) <= i)
    ++it;

  const Page *lastPage = *it;

  // Replace the requested packet and create new pages to replace the located pages.

  ByteVectorList packets = firstPage->packets();
  packets[i - firstPage->firstPacketIndex()] = packet;

  if(firstPage != lastPage && lastPage->packetCount() > 1) {
    ByteVectorList lastPagePackets = lastPage->packets();
    lastPagePackets.erase(lastPagePackets.begin());
    packets.append(lastPagePackets);
  }

  // TODO: This pagination method isn't accurate for what's being done here.
  // This should account for real possibilities like non-aligned packets and such.

  List<Page *> pages = Page::paginate(packets,
                                      Page::SinglePagePerGroup,
                                      firstPage->header()->streamSerialNumber(),
                                      firstPage->pageSequenceNumber(),
                                      firstPage->header()->firstPacketContinued(),
                                      lastPage->header()->lastPacketCompleted());
  pages.setAutoDelete(true);

  // Write the pages.

  ByteVector data;
  for(it = pages.begin(); it != pages.end(); ++it)
    data.append((*it)->render());

  const unsigned long originalOffset = firstPage->fileOffset();
  const unsigned long originalLength = lastPage->fileOffset() + lastPage->size() - originalOffset;

  insert(data, originalOffset, originalLength);

  // Renumber the following pages if the pages have been split or merged.

  const int numberOfNewPages
    = pages.back()->pageSequenceNumber() - lastPage->pageSequenceNumber();

  if(numberOfNewPages != 0) {
    long pageOffset = originalOffset + data.size();

    while(true) {
      Page page(this, pageOffset);
      if(!page.header()->isValid())
        break;

      page.setPageSequenceNumber(page.pageSequenceNumber() + numberOfNewPages);
      const ByteVector data = page.render();

      seek(pageOffset + 18);
      writeBlock(data.mid(18, 8));

      if(page.header()->lastPageOfStream())
        break;

      pageOffset += page.size();
    }
  }

  // Discard all the pages to keep them up-to-date by fetching them again.

  d->pages.clear();
}

bool Ogg::File::scanPhysicalPages()
{
  d->physicalPages.clear();
  d->logicalPages.clear();

  const long fileLength = length();
  long offset = 0;

  while(offset < fileLength) {
    seek(offset);
    ByteVector capture = readBlock(4);
    if(capture.size() != 4 || capture != ByteVector("OggS", 4)) {
      // Resync in case something unexpected is encountered.
      const long next = find("OggS", offset);
      if(next < 0)
        break;
      offset = next;
      continue;
    }

    PageHeader header(this, offset);
    if(!header.isValid())
      return false;

    const int pageSize = header.size() + header.dataSize();
    if(pageSize <= 0 || static_cast<long long>(offset) + pageSize > fileLength)
      return false;

    PhysicalPage *page = new PhysicalPage();
    page->offset = offset;
    page->size = pageSize;
    page->serial = header.streamSerialNumber();
    page->sequence = header.pageSequenceNumber();
    page->granule = header.absoluteGranularPosition();
    page->continued = header.firstPacketContinued();
    page->bos = header.firstPageOfStream();
    page->eos = header.lastPageOfStream();
    page->lastCompleted = header.lastPacketCompleted();
    page->packetSizes = header.packetSizes();

    d->physicalPages.append(page);
    offset += pageSize;
  }

  return !d->physicalPages.isEmpty();
}

ByteVector Ogg::File::readLogicalPacket(unsigned int i)
{
  // Find the first page that carries the requested packet, including a page
  // on which the packet is merely opened (its terminating lacing value is on
  // a later page).

  List<unsigned int>::ConstIterator pageIt = d->logicalPages.begin();
  PhysicalPage *page = 0;
  for(; pageIt != d->logicalPages.end(); ++pageIt) {
    page = d->physicalPages[*pageIt];
    // packetSizes() entries already include a final partial packet left open
    // at the end of the page, so the highest index touched here is simply
    // firstPacketIndex + entry count - 1.
    const int highestTouched = page->firstPacketIndex
                             + static_cast<int>(page->packetSizes.size()) - 1;
    if(static_cast<int>(i) >= page->firstPacketIndex &&
       static_cast<int>(i) <= highestTouched)
      break;
  }

  if(pageIt == d->logicalPages.end()) {
    debug("Ogg::File::readLogicalPacket() -- packet not found.");
    return ByteVector();
  }

  const int local = static_cast<int>(i) - page->firstPacketIndex;
  const int entries = static_cast<int>(page->packetSizes.size());
  const bool terminatesHere =
    page->lastCompleted || local < entries - 1;

  Page reader(this, page->offset);
  if(!reader.header()->isValid())
    return ByteVector();

  // Page::packets() also yields the partial (still open) packet as its last
  // entry, so the head is read the same way in both cases.
  ByteVector result = reader.packets()[local];
  bool open = !terminatesHere;

  while(open) {
    ++pageIt;
    if(pageIt == d->logicalPages.end())
      return ByteVector();

    PhysicalPage *next = d->physicalPages[*pageIt];
    Page continuation(this, next->offset);
    if(!continuation.header()->isValid() ||
       !continuation.header()->firstPacketContinued())
      return ByteVector();

    result.append(continuation.packets().front());

    // The continued packet spans yet another page only when this page has a
    // single entry and still ends with an open packet.  Otherwise its first
    // entry is the terminating chunk of the requested packet, even if another
    // packet is opened afterwards on the same page.
    open = (!next->lastCompleted && next->packetSizes.size() == 1);
  }

  return result;
}

bool Ogg::File::writeLogicalPacket(unsigned int i, const ByteVector &packet)
{
  // Find the page on which the requested packet starts.
  List<unsigned int>::Iterator firstIt = d->logicalPages.begin();
  PhysicalPage *firstPage = 0;
  for(; firstIt != d->logicalPages.end(); ++firstIt) {
    firstPage = d->physicalPages[*firstIt];
    const int lastIndex = firstPage->firstPacketIndex
                        + static_cast<int>(firstPage->packetSizes.size()) - 1;
    if(static_cast<int>(i) >= firstPage->firstPacketIndex &&
       static_cast<int>(i) <= lastIndex)
      break;
  }
  if(firstIt == d->logicalPages.end())
    return false;

  // The region runs from that start page up to and including the first page
  // on which no packet stays open.  This guarantees every packet touched by
  // the rewrite is wholly contained in the region (no packet is split across
  // the region boundary), while packets before the replaced packet and after
  // the region remain untouched.
  List<unsigned int>::Iterator lastIt = firstIt;
  PhysicalPage *lastPage = firstPage;
  while(!lastPage->lastCompleted) {
    ++lastIt;
    if(lastIt == d->logicalPages.end())
      return false;
    lastPage = d->physicalPages[*lastIt];
  }

  // Gather every complete logical packet carried by the pages in the region.
  // Whole packets are read instead of per page slices so this also works when
  // the replaced packet does not finish on the first page of the region.
  // Every packet other than the replaced one is preserved byte for byte.
  const int firstPacket = firstPage->firstPacketIndex;
  const int lastPacket = lastPage->firstPacketIndex
                       + static_cast<int>(lastPage->packetSizes.size()) - 1;
  if(firstPacket < 0 || lastPacket < firstPacket)
    return false;

  ByteVectorList packets;
  for(int p = firstPacket; p <= lastPacket; ++p) {
    if(static_cast<unsigned int>(p) == i)
      packets.append(packet);
    else {
      const ByteVector original =
        readLogicalPacket(static_cast<unsigned int>(p));
      if(original.isEmpty())
        return false;
      packets.append(original);
    }
  }

  List<Page *> rendered = Page::paginate(packets,
                                         Page::SinglePagePerGroup,
                                         d->serialNumber,
                                         firstPage->sequence,
                                         firstPage->continued,
                                         lastPage->lastCompleted);
  rendered.setAutoDelete(true);
  if(rendered.isEmpty())
    return false;

  ByteVector newBytes;
  for(List<Page *>::ConstIterator it = rendered.begin();
      it != rendered.end(); ++it)
    newBytes.append((*it)->render());

  // The region spans the physical area between the first and last involved
  // page of the logical stream.  Pages of other streams interleaved in there
  // have to survive byte for byte and keep their relative order.

  const unsigned int firstPhysIndex = *firstIt;
  const unsigned int lastPhysIndex  = *lastIt;
  const long regionStart = firstPage->offset;
  const long regionEnd   = lastPage->offset + lastPage->size;
  const long oldLength   = length();

  long long foreignBytes = 0;
  for(unsigned int p = firstPhysIndex; p <= lastPhysIndex; ++p) {
    if(!d->physicalPages[p]->logical)
      foreignBytes += d->physicalPages[p]->size;
  }

  const long long shift =
    static_cast<long long>(newBytes.size()) + foreignBytes
    - (regionEnd - regionStart);

  int logicalPagesInRegion = 0;
  for(unsigned int p = firstPhysIndex; p <= lastPhysIndex; ++p) {
    if(d->physicalPages[p]->logical)
      ++logicalPagesInRegion;
  }
  const int pageNumberDelta =
    static_cast<int>(rendered.size()) - logicalPagesInRegion;

  const long newRegionLength =
    static_cast<long>(foreignBytes) + static_cast<long>(newBytes.size());
  const long newLength = oldLength + static_cast<long>(shift);

  // Helper writing the new region: the preserved foreign pages first, in
  // their original relative order, then the freshly rendered target pages.
  // Foreign destinations are always ahead of their sources, so the forward
  // block copy never clobbers data still needed.
  //
  // The region content is rebuilt either before (negative shift) or after
  // (positive shift) the suffix is moved, so the suffix move can never
  // overwrite foreign pages that have not been copied yet.

  struct RegionWriter
  {
    static void run(Ogg::File *self,
                    const List<PhysicalPage *> &foreign,
                    long regionStart, long long foreignTotal,
                    const ByteVector &targetBytes,
                    unsigned long blockSize)
    {
      long foreignDestination = regionStart;
      for(List<PhysicalPage *>::ConstIterator it = foreign.begin();
          it != foreign.end(); ++it) {
        self->copyRange((*it)->offset, (*it)->size, foreignDestination,
                        blockSize);
        foreignDestination += (*it)->size;
      }
      self->seek(regionStart + static_cast<long>(foreignTotal));
      self->writeBlock(targetBytes);
    }
  };

  if(shift >= 0) {
    // Grow (or keep) the region: first push the suffix backwards, then
    // rebuild the region in the freed space.
    if(regionEnd < oldLength)
      copyRange(regionEnd, oldLength - regionEnd, regionEnd + shift,
                MoveBlockSize);

    List<PhysicalPage *> foreign;
    for(unsigned int p = firstPhysIndex; p <= lastPhysIndex; ++p)
      if(!d->physicalPages[p]->logical)
        foreign.append(d->physicalPages[p]);
    RegionWriter::run(this, foreign, regionStart, foreignBytes, newBytes,
                      MoveBlockSize);
  }
  else {
    // Shrink the region: rebuild it at the front first (this only occupies
    // bytes that used to hold region data), then pull the suffix forwards
    // into the freed tail.  The first block is made as large as the gap so
    // the writer can never overtake the reader.
    List<PhysicalPage *> foreign;
    for(unsigned int p = firstPhysIndex; p <= lastPhysIndex; ++p)
      if(!d->physicalPages[p]->logical)
        foreign.append(d->physicalPages[p]);
    RegionWriter::run(this, foreign, regionStart, foreignBytes, newBytes,
                      MoveBlockSize);

    if(regionEnd < oldLength) {
      const unsigned long gap =
        static_cast<unsigned long>(regionEnd - (regionStart + newRegionLength));
      const unsigned long blockSize =
        gap > MoveBlockSize ? gap : MoveBlockSize;
      copyRange(regionEnd, oldLength - regionEnd,
                regionStart + newRegionLength, blockSize);
    }
  }

  // Renumber the selected stream's remaining pages and recompute their
  // checksums.  Foreign pages are not touched at all.
  long offset = regionStart + newRegionLength;
  while(offset < newLength) {
    PageHeader header(this, offset);
    if(!header.isValid())
      return false;
    const int pageSize = header.size() + header.dataSize();

    if(header.streamSerialNumber() == d->serialNumber) {
      seek(offset);
      ByteVector pageBytes = readBlock(pageSize);
      if(pageBytes.size() != pageSize)
        return false;

      const unsigned int newSequence =
        header.pageSequenceNumber() + pageNumberDelta;
      const ByteVector sequenceBytes = ByteVector::fromUInt(newSequence, false);
      std::copy(sequenceBytes.begin(), sequenceBytes.end(),
                pageBytes.begin() + 18);
      std::fill(pageBytes.begin() + 22, pageBytes.begin() + 26, '\0');
      const ByteVector checksum =
        ByteVector::fromUInt(pageBytes.checksum(), false);
      std::copy(checksum.begin(), checksum.end(), pageBytes.begin() + 22);

      seek(offset + 18);
      writeBlock(pageBytes.mid(18, 8));
    }

    offset += pageSize;
  }

  if(shift < 0)
    truncate(newLength);

  invalidatePageIndex();
  return true;
}

void Ogg::File::copyRange(long source, long long length, long destination,
                          unsigned long blockSize)
{
  if(length <= 0 || source == destination)
    return;

  if(destination < source) {
    // Forward copy.  The write head always trails the read head by the
    // constant gap (source - destination), so every block must be no larger
    // than that gap; otherwise a write could overwrite a source block that
    // has not been read yet.  A small gap simply means more, smaller blocks.
    const long gap = source - destination;
    long long remaining = length;
    long from = source;
    long to = destination;

    while(remaining > 0) {
      const unsigned long limit = static_cast<unsigned long>(
        std::min<long long>(blockSize, gap));
      const unsigned long thisBlock = static_cast<unsigned long>(
        std::min<long long>(remaining, limit));
      seek(from);
      const ByteVector block = readBlock(thisBlock);
      if(block.size() != thisBlock) {
        debug("Ogg::File::copyRange() -- short read.");
        return;
      }
      seek(to);
      writeBlock(block);
      from += static_cast<long>(thisBlock);
      to += static_cast<long>(thisBlock);
      remaining -= thisBlock;
    }
  }
  else {
    // Backward copy for moves towards the end of the file.
    long long remaining = length;
    unsigned long firstBlock = static_cast<unsigned long>(
      std::min<long long>(blockSize, destination - source));
    if(firstBlock == 0)
      firstBlock = blockSize;

    while(remaining > 0) {
      const unsigned long thisBlock = static_cast<unsigned long>(
        std::min<long long>(remaining, firstBlock));
      const long from = source + remaining - thisBlock;
      const long to = destination + remaining - thisBlock;
      seek(from);
      const ByteVector block = readBlock(thisBlock);
      if(block.size() != thisBlock) {
        debug("Ogg::File::copyRange() -- short read.");
        return;
      }
      seek(to);
      writeBlock(block);
      remaining -= thisBlock;
      firstBlock = blockSize;
    }
  }
}
