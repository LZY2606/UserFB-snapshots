/*
* ResourceDirectory.cpp - Part of the PeLib library.
*
* Copyright (c) 2004 - 2005 Sebastian Porst (webmaster@the-interweb.com)
* All rights reserved.
*
* This software is licensed under the zlib/libpng License.
* For more details see http://www.opensource.org/licenses/zlib-license.php
* or the license information file (license.htm) in the root directory
* of PeLib.
*/

#ifndef RESOURCEDIRECTORY_H
#define RESOURCEDIRECTORY_H

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include "pelib/PeLibInc.h"
#include "pelib/PeHeader.h"

namespace PeLib
{
	class ResourceElement;
	class ResourceDirectory;
	template <int bits> class ResourceDirectoryT;
	template <int bits> struct ResourceReader;

	/// A contiguous range of RVAs and the file offset where its raw bytes start.
	struct ResourceImageMapping
	{
		/// First RVA of the mapping.
		std::uint64_t rvaBegin;
		/// One past the last RVA of the mapping.
		std::uint64_t rvaEnd;
		/// File offset of the byte at rvaBegin.
		std::uint64_t fileBegin;
	};

	/// Builds the RVA-to-file mapping of an image for the resource reader.
	template <int bits>
	struct ResourceReader
	{
		/// Returns the ranges of RVAs that are backed by real file bytes.
		static std::vector<ResourceImageMapping> buildImageMappings(
				const PeHeaderT<bits>& peHeader,
				std::uint64_t uiFileSize)
		{
			std::vector<ResourceImageMapping> mappings;

			// RVAs below SizeOfHeaders are mapped one-to-one onto the file, but
			// only as far as real file bytes exist.
			const std::uint64_t uiHeadersSize =
					std::min<std::uint64_t>(peHeader.getSizeOfHeaders(), uiFileSize);
			if (uiHeadersSize > 0)
			{
				mappings.push_back({0, uiHeadersSize, 0});
			}

			// Sections are not assumed to be sorted by virtual address. Only
			// their raw bytes are mapped; the zero-filled tail implied by
			// VirtualSize is not resource content.
			for (word i = 0; i < peHeader.calcNumberOfSections(); ++i)
			{
				const std::uint64_t uiRawSize = peHeader.getSizeOfRawData(i);
				const std::uint64_t uiRawOffset = peHeader.getPointerToRawData(i);
				if (uiRawSize == 0 || uiRawOffset == 0 || uiRawOffset >= uiFileSize)
				{
					continue;
				}

				const std::uint64_t uiVirtualAddress = peHeader.getVirtualAddress(i);
				const std::uint64_t uiAvailableRawSize =
						std::min(uiRawSize, uiFileSize - uiRawOffset);
				if (uiAvailableRawSize == 0)
				{
					continue;
				}

				mappings.push_back({
					uiVirtualAddress,
					uiVirtualAddress + uiAvailableRawSize,
					uiRawOffset
				});
			}

			std::sort(
					mappings.begin(),
					mappings.end(),
					[](const ResourceImageMapping& left, const ResourceImageMapping& right) {
						return left.rvaBegin < right.rvaBegin;
					}
			);

			return mappings;
		}
	};

	/// The class ResourceChild is used to store information about a resource node.
	class ResourceChild
	{
		friend class ResourceElement;
		friend class ResourceDirectory;
		friend class ResourceNode;
		friend class ResourceLeaf;
		template <int bits> friend class ResourceDirectoryT;

		/// Stores name and offset of a resource node.
		PELIB_IMG_RES_DIR_ENTRY entry;
		/// A pointer to one of the node's child nodes.
		ResourceElement* child;

		public:
		  /// Function which compares a resource ID to the node's resource ID.
		  bool equalId(dword wId) const; // EXPORT
		  /// Function which compares a string to the node's resource name.
		  bool equalName(std::string strName) const; // EXPORT
		  /// Predicate that determines if a child is identified by name or by ID.
		  bool isNamedResource() const; // EXPORT
		  /// Used for sorting a node's children.
		  bool operator<(const ResourceChild& rc) const; // EXPORT

		  /// A comparison function for searching a resource element by its ID.
		  bool hasEqual(dword id) const { return equalId(id); }
		  /// A comparison function for searching a resource element by its name.
		  bool hasEqual(const std::string& name) const { return equalName(name); }

		  /// Returns the node's number of children.
		  unsigned int getNumberOfChildren() const; // EXPORT
		  /// Returns a child of this child.
		  ResourceChild* getChildOfThisChild(std::size_t uiIndex); // EXPORT
		  const ResourceChild* getChildOfThisChild(std::size_t uiIndex) const; // EXPORT

		  /// Returns a pointer to ResourceElement.
		  ResourceElement* getNode();
		  const ResourceElement* getNode() const;
		  /// Sets a pointer to ResourceElement.
		  void setNode(ResourceElement* node);

		  /// Returns the name of the node.
		  std::string getName() const; // EXPORT
		  /// Returns the Name value of the node.
		  dword getOffsetToName() const; // EXPORT
		  /// Returns the OffsetToData value of the node.
		  dword getOffsetToData() const; // EXPORT

		  /// Sets the name of the node.
		  void setName(const std::string& strNewName); // EXPORT
		  /// Sets the Name value of the node.
		  void setOffsetToName(dword dwNewOffset); // EXPORT
		  /// Sets the OffsetToData value of the node.
		  void setOffsetToData(dword dwNewOffset); // EXPORT

		  /// Returns the size of a resource child.
//		unsigned int size() const;

		  /// Standard constructor. Does absolutely nothing.
		  ResourceChild();
		  /// Makes a deep copy of a ResourceChild object.
		  ResourceChild(const ResourceChild& rhs);
		  /// Makes a deep copy of a ResourceChild object.
		  ResourceChild& operator=(const ResourceChild& rhs);
		  /// Deletes a ResourceChild object.
		  ~ResourceChild();
	};

	/// Base class for ResourceNode and ResourceLeaf, the elements of the resource tree.
	/// \todo write
	class ResourceElement
	{
		friend class ResourceChild;
		friend class ResourceNode;
		friend class ResourceLeaf;
		friend class ResourceDirectory;

		protected:
		  /// Stores RVA of the resource element in the file.
		  unsigned int uiElementRva;

		  /// Reads the next resource element from the input stream.
		  virtual int read(std::istream&, unsigned int, ResourceDirectory*) = 0;
		  /// Writes the next resource element into the OutputBuffer.
		  virtual void rebuild(OutputBuffer&, unsigned int, unsigned int, const std::string&) const = 0;
		  /// Recalculates the tree for different RVA.
		  virtual void recalculate(unsigned int& uiCurrentOffset, unsigned int uiNewRva) = 0;

		public:
		  /// Returns the RVA of the element in the file.
		  unsigned int getElementRva() const; // EXPORT
		  /// Indicates if the resource element is a leaf or a node.
		  virtual bool isLeaf() const = 0; // EXPORT
		  /// Corrects erroneous values in the ResourceElement.
		  virtual void makeValid() = 0; // EXPORT
		  /// Returns the size of a resource element.
//		  virtual unsigned int size() const = 0;
		  /// Constructor
		  ResourceElement();
		  /// Necessary virtual destructor.
		  virtual ~ResourceElement() {}
	};

	/// ResourceLeafs represent the leafs of the resource tree: The actual resources.
	class ResourceLeaf : public ResourceElement
	{
		friend class ResourceChild;
		friend class ResourceDirectory;
		template <typename T> friend struct fixNumberOfEntries;
		template <int bits> friend class ResourceDirectoryT;

		private:
		  /// The resource data.
		  std::vector<byte> m_data;
		  /// PeLib equivalent of the Win32 structure IMAGE_RESOURCE_DATA_ENTRY
		  PELIB_IMAGE_RESOURCE_DATA_ENTRY entry;

		protected:
		  int read(std::istream& inStream, unsigned int uiOffset, ResourceDirectory* resDir);
		  /// Writes the next resource leaf into the OutputBuffer.
		  void rebuild(OutputBuffer&, unsigned int uiOffset, unsigned int uiRva, const std::string&) const;
		  /// Recalculates the tree for different RVA.
		  virtual void recalculate(unsigned int& uiCurrentOffset, unsigned int uiNewRva) override;

		public:
		  /// Indicates if the resource element is a leaf or a node.
		  bool isLeaf() const; // EXPORT
		  /// Corrects erroneous values in the ResourceLeaf.
		  void makeValid(); // EXPORT
		  /// Reads the next resource leaf from the InputBuffer.
		  /// Returns the size of a resource lead.
//		  unsigned int size() const;

		  /// Returns the resource data of this resource leaf.
		  std::vector<byte> getData() const; // EXPORT
		  /// Sets the resource data of this resource leaf.
		  void setData(const std::vector<byte>& vData); // EXPORT

		  /// Returns the OffsetToData value of this resource leaf.
		  dword getOffsetToData() const; // EXPORT
		  /// Returns the Size value of this resource leaf.
		  dword getSize() const; // EXPORT
		  /// Returns the CodePage value of this resource leaf.
		  dword getCodePage() const; // EXPORT
		  /// Returns the Reserved value of this resource leaf.
		  dword getReserved() const; // EXPORT

		  /// Sets the OffsetToData value of this resource leaf.
		  void setOffsetToData(dword dwValue); // EXPORT
		  /// Sets the Size value of this resource leaf.
		  void setSize(dword dwValue); // EXPORT
		  /// Sets the CodePage value of this resource leaf.
		  void setCodePage(dword dwValue); // EXPORT
		  /// Sets the Reserved value of this resource leaf.
		  void setReserved(dword dwValue); // EXPORT
		  /// Constructor
		  ResourceLeaf();
		  /// Destructor
		  virtual ~ResourceLeaf() override;
	};

	/// ResourceNodes represent the nodes in the resource tree.
	class ResourceNode : public ResourceElement
	{
		friend class ResourceChild;
		friend class ResourceDirectory;
		template <typename T> friend struct fixNumberOfEntries;
		template <int bits> friend class ResourceDirectoryT;

		/// The node's children.
		std::vector<ResourceChild> children;
		/// The node's header. Equivalent to IMAGE_RESOURCE_DIRECTORY from the Win32 API.
		PELIB_IMAGE_RESOURCE_DIRECTORY header;

		protected:
		  /// Reads the next resource node.
		  int read(std::istream& inStream, unsigned int uiOffset, ResourceDirectory* resDir);
		  /// Writes the next resource node into the OutputBuffer.
		  void rebuild(OutputBuffer&, unsigned int uiOffset, unsigned int uiRva, const std::string&) const;
		  /// Recalculates the tree for different RVA.
		  virtual void recalculate(unsigned int& uiCurrentOffset, unsigned int uiNewRva) override;

		public:
		  /// Indicates if the resource element is a leaf or a node.
		  bool isLeaf() const; // EXPORT
		  /// Corrects erroneous values in the ResourceNode.
		  void makeValid(); // EXPORT

		  /// Returns the node's number of children.
		  unsigned int getNumberOfChildren() const; // EXPORT
		  /// Adds another child to node.
		  ResourceChild* addChild(); // EXPORT
		  /// Returns a node's child.
		  ResourceChild* getChild(std::size_t uiIndex); // EXPORT
		  const ResourceChild* getChild(std::size_t uiIndex) const; // EXPORT
		  /// Removes a node's child.
		  void removeChild(unsigned int uiIndex); // EXPORT

		  /// Returns the name of one of the node's children.
		  std::string getChildName(unsigned int uiIndex) const; // EXPORT
		  /// Returns the Name value of one of the node's children.
		  dword getOffsetToChildName(unsigned int uiIndex) const; // EXPORT
		  /// Returns the OffsetToData value of one of the node's children.
		  dword getOffsetToChildData(unsigned int uiIndex) const; // EXPORT

		  /// Sets the name of one of the node's children.
		  void setChildName(unsigned int uiIndex, const std::string& strNewName); // EXPORT
		  /// Sets the Name value of one of the node's children.
		  void setOffsetToChildName(unsigned int uiIndex, dword dwNewOffset); // EXPORT
		  /// Sets the OffsetToData value of one of the node's children.
		  void setOffsetToChildData(unsigned int uiIndex, dword dwNewOffset); // EXPORT

		  /// Returns the node's Characteristics value.
		  dword getCharacteristics() const; // EXPORT
		  /// Returns the node's TimeDateStamp value.
		  dword getTimeDateStamp() const; // EXPORT
		  /// Returns the node's MajorVersion value.
		  word getMajorVersion() const; // EXPORT
		  /// Returns the node's MinorVersion value.
		  word getMinorVersion() const; // EXPORT
		  /// Returns the node's NumberOfNamedEntries value.
		  word getNumberOfNamedEntries() const; // EXPORT
		  /// Returns the node's NumberOfIdEntries value.
		  word getNumberOfIdEntries() const; // EXPORT

		  /// Sets the node's Characteristics value.
		  void setCharacteristics(dword value); // EXPORT
		  /// Sets the node's TimeDateStamp value.
		  void setTimeDateStamp(dword value); // EXPORT
		  /// Sets the node's MajorVersion value.
		  void setMajorVersion(word value); // EXPORT
		  /// Sets the node's MinorVersion value.
		  void setMinorVersion(word value); // EXPORT
		  /// Sets the node's NumberOfNamedEntries value.
		  void setNumberOfNamedEntries(word value); // EXPORT
		  /// Sets the node's NumberOfIdEntries value.
		  void setNumberOfIdEntries(word value); // EXPORT

		  /// Returns the size of a resource node.
//		unsigned int size() const;

		  /// Constructor
		  ResourceNode();
		  /// Destructor
		  virtual ~ResourceNode() override;
	};

	/// Unspecialized function that's used as base template for the specialized versions below.
	template<typename T>
	struct fixNumberOfEntries
	{
		/// Fixes a resource node's header.
		static void fix(ResourceNode*);
	};

	/// Fixes NumberOfIdEntries value of a node.
	template<>
	struct fixNumberOfEntries<dword>
	{
		/// Fixes a resource node's NumberOfIdEntries value.
		static void fix(ResourceNode* node)
		{
			node->header.NumberOfIdEntries = static_cast<PeLib::word>(
				node->children.size() - std::count_if(
						node->children.begin(),
						node->children.end(),
						[](const auto& i) { return i.isNamedResource(); }
				)
			);
		}
	};

	/// Fixes NumberOfNamedEntries value of a node.
	template<>
	struct fixNumberOfEntries<std::string>
	{
		/// Fixes a resource node's NumberOfNamedEntries value.
		static void fix(ResourceNode* node)
		{
			node->header.NumberOfNamedEntries = static_cast<PeLib::word>(
				std::count_if(
					node->children.begin(),
					node->children.end(),
					[](const auto& i) { return i.isNamedResource(); }
				)
			);
		}
	};

	/// Class that represents the resource directory of a PE file.
	/**
	* The class ResourceDirectory represents the resource directory of a PE file. This class is fundamentally
	* different from the other classes of the PeLib library due to the structure of the ResourceDirectory.
	* For once, it's possible to manipulate the ResourceDirectory through a set of "high level" functions and
	* and through a set of "low level" functions. The "high level" functions are the functions inside the
	* ResourceDirectory class with the exception of getRoot.<br><br>
	* getRoot on the other hand is the first "low level" function. Use it to retrieve the root node of the
	* resource tree. Then you can traverse through the tree and manipulate individual nodes and leafs
	* directly using the functions provided by the classes ResourceNode and ResourceLeaf.<br><br>
	* There's another difference between the ResourceDirectory class and the other PeLib classes, which is
	* once again caused by the special structure of the PE resource directory. The nodes of the resource
	* tree must be in a certain order. Manipulating the resource tree does not directly sort the nodes
	* correctly as this would cause more trouble than it fixes. That means it's your responsibility to
	* fix the resource tree after manipulating it. PeLib makes the job easy for you, just call the
	* ResourceDirectory<bits>::makeValid function.<br><br>
	* You might also wonder why there's no size() function in this class. I did not forget it. It's just
	* that it's impossible to calculate the size of the resource directory without rebuilding it. So why
	* should PeLib do this if you can do it just as easily by calling rebuild() and then checking the length
	* of the returned vector.<br><br>
	* There are also different ways to serialize (rebuild) the resource tree as it's not a fixed structure
	* that can easily be minimized like most other PE directories.<br><br>
	* This means it's entirely possible that the resource tree you read from a file differs from the one
	* PeLib creates. This might cause a minor issue. The original resource tree might be smaller (due to
	* different padding) so it's crucial that you check if there's enough space in the original resource
	* directory before you write the rebuilt resource directory back to the file.
	**/
	class ResourceDirectory
	{
		friend class ResourceNode;
		friend class ResourceLeaf;

		protected:
		  /// Start offset of directory in file.
		  unsigned int m_readOffset;
		  /// The root node of the resource directory.
		  ResourceNode m_rnRoot;
		  /// Detection of invalid structure of nodes in directory.
		  std::set<std::size_t> m_resourceNodeOffsets;
		  /// Stores RVAs which are occupied by this export directory.
		  std::vector<std::pair<unsigned int, unsigned int>> m_occupiedAddresses;
		  /// Error detected by the import table parser
		  LoaderError m_ldrError;

		  /// RVA ranges of the image that are backed by real file bytes.
		  std::vector<ResourceImageMapping> m_imageMappings;
		  /// Size of the image file that is currently being read.
		  std::uint64_t m_imageFileSize;
		  /// SizeOfImage of the image that is currently being read.
		  unsigned int m_sizeOfImage;
		  /// RVA of the resource directory that is currently being read.
		  unsigned int m_resourceDirRva;
		  /// Relative directory offsets on the path from the root to the current node.
		  std::vector<unsigned int> m_directoryAncestors;

		  /// Reads a byte range from the image through the RVA-to-file mapping.
		  int readMappedBytes(
				  std::istream& inStream,
				  std::uint64_t uiRva,
				  std::uint64_t uiSize,
				  std::vector<byte>& vData) const;

		  /// Converts an RVA to a file offset using the image mappings.
		  bool mapRvaToFileOffset(std::uint64_t uiRva, std::uint64_t& uiFileOffset) const;

		  /// Records an occupied RVA range, clamping malformed ends to the last RVA.
		  void addOccupiedAddressRangeU64(std::uint64_t start, std::uint64_t end);

		  /// Resets all state that belongs to a single read.
		  void resetReadState();

		  /// Checks if a directory offset is on the current ancestor path.
		  bool isDirectoryAncestor(unsigned int uiOffset) const;
		  /// Adds a directory offset to the current ancestor path.
		  void pushDirectoryAncestor(unsigned int uiOffset);
		  /// Removes the last directory offset from the current ancestor path.
		  void popDirectoryAncestor();

		  /// Follows the first branch from an element down to its first leaf.
		  static const ResourceLeaf* firstLeaf(const ResourceElement* element);
		  /// Follows the first branch from an element down to its first leaf.
		  static ResourceLeaf* firstLeaf(ResourceElement* element);

		  // Prepare for some crazy syntax below to make Digital Mars happy.

		  /// Retrieves an iterator to a specified resource child.
		  template<typename S, typename T>
		  std::vector<ResourceChild>::const_iterator locateResourceT(S restypeid, T resid) const;

		  /// Retrieves an iterator to a specified resource child.
		  template<typename S, typename T>
		  std::vector<ResourceChild>::iterator locateResourceT(S restypeid, T resid);

		  /// Adds a new resource.
		  template<typename S, typename T>
		  int addResourceT(S restypeid, T resid, ResourceChild& rc);

		  /// Removes new resource.
		  template<typename S, typename T>
		  int removeResourceT(S restypeid, T resid);

		  /// Returns the data of a resource.
		  template<typename S, typename T>
		  int getResourceDataT(S restypeid, T resid, std::vector<byte>& data) const;

		  /// Sets the data of a resource.
		  template<typename S, typename T>
		  int setResourceDataT(S restypeid, T resid, std::vector<byte>& data);

		  /// Returns the ID of a resource.
		  template<typename S, typename T>
		  dword getResourceIdT(S restypeid, T resid) const;

		  /// Sets the ID of a resource.
		  template<typename S, typename T>
		  int setResourceIdT(S restypeid, T resid, dword dwNewResId);

		  /// Returns the name of a resource.
		  template<typename S, typename T>
		  std::string getResourceNameT(S restypeid, T resid) const;

		  /// Sets the name of a resource.
		  template<typename S, typename T>
		  int setResourceNameT(S restypeid, T resid, std::string strNewResName);

		public:
		  /// Constructor
		  ResourceDirectory();
		  /// Destructor
		  virtual ~ResourceDirectory() = default;

		  ResourceNode* getRoot();
		  const ResourceNode* getRoot() const;

		  /// Retrieve the loader error
		  LoaderError loaderError() const;
		  void setLoaderError(LoaderError ldrError);

		  /// Corrects a erroneous resource directory.
		  void makeValid();
		  /// Rebuilds the resource directory.
		  void rebuild(std::vector<byte>& vBuffer, unsigned int uiRva) const;
		  /// Recalculate the tree for different RVA
		  void recalculate(unsigned int& uiNewSize, unsigned int uiNewRva);
		  /// Returns the size of the rebuilt resource directory.
//		  unsigned int size() const;
		  /// Writes the resource directory to a file.
		  int write(const std::string& strFilename, unsigned int uiOffset, unsigned int uiRva) const;

		  /// Adds a new resource type.
		  int addResourceType(dword dwResTypeId);
		  /// Adds a new resource type.
		  int addResourceType(const std::string& strResTypeName);

		  /// Removes a resource type and all of it's resources.
		  int removeResourceType(dword dwResTypeId);
		  /// Removes a resource type and all of it's resources.
		  int removeResourceType(const std::string& strResTypeName);

		  /// Removes a resource type and all of it's resources.
		  int removeResourceTypeByIndex(unsigned int uiIndex);

		  /// Adds a new resource.
		  int addResource(dword dwResTypeId, dword dwResId);
		  /// Adds a new resource.
		  int addResource(dword dwResTypeId, const std::string& strResName);
		  /// Adds a new resource.
		  int addResource(const std::string& strResTypeName, dword dwResId);
		  /// Adds a new resource.
		  int addResource(const std::string& strResTypeName, const std::string& strResName);

		  /// Removes a resource.
		  int removeResource(dword dwResTypeId, dword dwResId);
		  /// Removes a resource.
		  int removeResource(dword dwResTypeId, const std::string& strResName);
		  /// Removes a resource.
		  int removeResource(const std::string& strResTypeName, dword dwResId);
		  /// Removes a resource.
		  int removeResource(const std::string& strResTypeName, const std::string& strResName);

		  /// Returns start offset of resource directory in file.
		  unsigned int getOffset() const;

		  /// Returns the number of resource types.
		  unsigned int getNumberOfResourceTypes() const;

		  /// Returns the ID of a resource type.
		  dword getResourceTypeIdByIndex(unsigned int uiIndex) const;
		  /// Returns the name of a resource type.
		  std::string getResourceTypeNameByIndex(unsigned int uiIndex) const;

		  /// Converts a resource type ID to an index.
		  int resourceTypeIdToIndex(dword dwResTypeId) const;
		  /// Converts a resource type name to an index.
		  int resourceTypeNameToIndex(const std::string& strResTypeName) const;

		  /// Returns the number of resources of a certain resource type.
		  unsigned int getNumberOfResources(dword dwId) const;
		  /// Returns the number of resources of a certain resource type.
		  unsigned int getNumberOfResources(const std::string& strResTypeName) const;

		  /// Returns the number of resources of a certain resource type.
		  unsigned int getNumberOfResourcesByIndex(unsigned int uiIndex) const;

		  /// Returns the data of a certain resource.
		  void getResourceData(dword dwResTypeId, dword dwResId, std::vector<byte>& data) const;
		  /// Returns the data of a certain resource.
		  void getResourceData(dword dwResTypeId, const std::string& strResName, std::vector<byte>& data) const;
		  /// Returns the data of a certain resource.
		  void getResourceData(const std::string& strResTypeName, dword dwResId, std::vector<byte>& data) const;
		  /// Returns the data of a certain resource.
		  void getResourceData(const std::string& strResTypeName, const std::string& strResName, std::vector<byte>& data) const;

		  /// Returns the data of a certain resource.
		  void getResourceDataByIndex(unsigned int uiResTypeIndex, unsigned int uiResIndex, std::vector<byte>& data) const;

		  /// Sets the data of a certain resource.
		  void setResourceData(dword dwResTypeId, dword dwResId, std::vector<byte>& data);
		  /// Sets the data of a certain resource.
		  void setResourceData(dword dwResTypeId, const std::string& strResName, std::vector<byte>& data);
		  /// Sets the data of a certain resource.
		  void setResourceData(const std::string& strResTypeName, dword dwResId, std::vector<byte>& data);
		  /// Sets the data of a certain resource.
		  void setResourceData(const std::string& strResTypeName, const std::string& strResName, std::vector<byte>& data);

		  /// Sets the data of a certain resource.
		  void setResourceDataByIndex(unsigned int uiResTypeIndex, unsigned int uiResIndex, std::vector<byte>& data);

		  /// Returns the ID of a certain resource.
		  dword getResourceId(dword dwResTypeId, const std::string& strResName) const;
		  /// Returns the ID of a certain resource.
		  dword getResourceId(const std::string& strResTypeName, const std::string& strResName) const;

		  /// Returns the ID of a certain resource.
		  dword getResourceIdByIndex(unsigned int uiResTypeIndex, unsigned int uiResIndex) const;

		  /// Sets the ID of a certain resource.
		  void setResourceId(dword dwResTypeId, dword dwResId, dword dwNewResId);
		  /// Sets the ID of a certain resource.
		  void setResourceId(dword dwResTypeId, const std::string& strResName, dword dwNewResId);
		  /// Sets the ID of a certain resource.
		  void setResourceId(const std::string& strResTypeName, dword dwResId, dword dwNewResId);
		  /// Sets the ID of a certain resource.
		  void setResourceId(const std::string& strResTypeName, const std::string& strResName, dword dwNewResId);

		  /// Sets the ID of a certain resource.
		  void setResourceIdByIndex(unsigned int uiResTypeIndex, unsigned int uiResIndex, dword dwNewResId);

		  /// Returns the name of a certain resource.
		  std::string getResourceName(dword dwResTypeId, dword dwResId) const;
		  /// Returns the name of a certain resource.
		  std::string getResourceName(const std::string& strResTypeName, dword dwResId) const;

		  /// Returns the name of a certain resource.
		  std::string getResourceNameByIndex(unsigned int uiResTypeIndex, unsigned int uiResIndex) const;

		  /// Sets the name of a certain resource.
		  void setResourceName(dword dwResTypeId, dword dwResId, const std::string& strNewResName);
		  /// Sets the name of a certain resource.
		  void setResourceName(dword dwResTypeId, const std::string& strResName, const std::string& strNewResName);
		  /// Sets the name of a certain resource.
		  void setResourceName(const std::string& strResTypeName, dword dwResId, const std::string& strNewResName);
		  /// Sets the name of a certain resource.
		  void setResourceName(const std::string& strResTypeName, const std::string& strResName, const std::string& strNewResName);

		  /// Sets the name of a certain resource.
		  void setResourceNameByIndex(unsigned int uiResTypeIndex, unsigned int uiResIndex, const std::string& strNewResName);

		  /// Insert offset of loaded node.
		  void insertNodeOffset(std::size_t nodeOffset);
		  /// Check if node with specified offset was loaded.
		  bool hasNodeOffset(std::size_t nodeOffset) const;

		  void addOccupiedAddressRange(unsigned int start, unsigned int end);
		  const std::vector<std::pair<unsigned int, unsigned int>>& getOccupiedAddresses() const;
	};

	/**
	* Looks through the entire resource tree and returns a const_iterator to the resource specified
	* by the parameters.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	* @return A const_iterator to the specified resource.
	**/
	template<typename S, typename T>
	std::vector<ResourceChild>::const_iterator ResourceDirectory::locateResourceT(S restypeid, T resid) const
	{
		auto Iter = std::find_if(
				m_rnRoot.children.begin(),
				m_rnRoot.children.end(),
				[&](const auto& res) { return res.hasEqual(restypeid); }
		);
		if (Iter == m_rnRoot.children.end())
		{
			return Iter;
		}

		ResourceNode* currNode = static_cast<ResourceNode*>(Iter->child);
		auto ResIter = std::find_if(
				currNode->children.begin(),
				currNode->children.end(),
				[&](const auto& res) { return res.hasEqual(resid); }
		);
		if (ResIter == currNode->children.end())
		{
			return ResIter;
		}

		return ResIter;
	}

	/**
	* Looks through the entire resource tree and returns an iterator to the resource specified
	* by the parameters.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	* @return An iterator to the specified resource.
	**/
	template<typename S, typename T>
	std::vector<ResourceChild>::iterator ResourceDirectory::locateResourceT(S restypeid, T resid)
	{
		auto Iter = std::find_if(
				m_rnRoot.children.begin(),
				m_rnRoot.children.end(),
				[&](const auto& res) { return res.hasEqual(restypeid); }
		);
		if (Iter == m_rnRoot.children.end())
		{
			return Iter;
		}

		ResourceNode* currNode = static_cast<ResourceNode*>(Iter->child);
		auto ResIter = std::find_if(
				currNode->children.begin(),
				currNode->children.end(),
				[&](const auto& res) { return res.hasEqual(resid); }
		);
		if (ResIter == currNode->children.end())
		{
			return ResIter;
		}

		return ResIter;
	}

	/**
	* Adds a new resource, resource type and ID are specified by the parameters.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	* @param rc ResourceChild that will be added.
	**/
	template<typename S, typename T>
	int ResourceDirectory::addResourceT(S restypeid, T resid, ResourceChild& rc)
	{
		auto Iter = std::find_if(
				m_rnRoot.children.begin(),
				m_rnRoot.children.end(),
				[&](const auto& res) { return res.hasEqual(restypeid); }
		);
		if (Iter == m_rnRoot.children.end())
		{
			return ERROR_ENTRY_NOT_FOUND;
			// throw Exceptions::ResourceTypeDoesNotExist(ResourceDirectoryId, __LINE__);
		}

		ResourceNode* currNode = static_cast<ResourceNode*>(Iter->child);
		auto ResIter = std::find_if(
				currNode->children.begin(),
				currNode->children.end(),
				[&](const auto& res) { return res.hasEqual(resid); }
		);
		if (ResIter != currNode->children.end())
		{
			return ERROR_DUPLICATE_ENTRY;
//			throw Exceptions::EntryAlreadyExists(ResourceDirectoryId, __LINE__);
		}

		rc.child = new ResourceNode;
		ResourceChild rlnew;
		rlnew.child = new ResourceLeaf;
		ResourceNode* currNode2 = static_cast<ResourceNode*>(rc.child);
		currNode2->children.push_back(rlnew);
		currNode->children.push_back(rc);

		fixNumberOfEntries<T>::fix(currNode);
		fixNumberOfEntries<T>::fix(currNode2);

		return ERROR_NONE;
	}

	/**
	* Removes a resource, resource type and ID are specified by the parameters.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	**/
	template<typename S, typename T>
	int ResourceDirectory::removeResourceT(S restypeid, T resid)
	{
		auto Iter = std::find_if(
				m_rnRoot.children.begin(),
				m_rnRoot.children.end(),
				[&](const auto& res) { return res.hasEqual(restypeid); }
		);
		if (Iter == m_rnRoot.children.end())
		{
			return ERROR_ENTRY_NOT_FOUND;
			//throw Exceptions::ResourceTypeDoesNotExist(ResourceDirectoryId, __LINE__);
		}

		ResourceNode* currNode = static_cast<ResourceNode*>(Iter->child);
		auto ResIter = std::find_if(
				currNode->children.begin(),
				currNode->children.end(),
				[&](const auto& res) { return res.hasEqual(resid); }
		);
		if (ResIter == currNode->children.end())
		{
			return ERROR_ENTRY_NOT_FOUND;
			// throw Exceptions::InvalidName(ResourceDirectoryId, __LINE__);
		}

		currNode->children.erase(ResIter);

		fixNumberOfEntries<T>::fix(currNode);

		return ERROR_NONE;
	}

	/**
	* Returns the data of a resource, resource type and ID are specified by the parameters.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	* @param data The data of the resource will be written into this vector.
	**/
	template<typename S, typename T>
	int ResourceDirectory::getResourceDataT(S restypeid, T resid, std::vector<byte>& data) const
	{
		data.clear();

		auto TypeIter = std::find_if(
				m_rnRoot.children.begin(),
				m_rnRoot.children.end(),
				[&](const auto& res) { return res.hasEqual(restypeid); }
		);
		if (TypeIter == m_rnRoot.children.end() || !TypeIter->child)
		{
			return ERROR_ENTRY_NOT_FOUND;
		}

		const ResourceNode* currNode = dynamic_cast<const ResourceNode*>(TypeIter->child);
		if (!currNode)
		{
			return ERROR_ENTRY_NOT_FOUND;
		}

		auto ResIter = std::find_if(
				currNode->children.begin(),
				currNode->children.end(),
				[&](const auto& res) { return res.hasEqual(resid); }
		);
		if (ResIter == currNode->children.end())
		{
			return ERROR_ENTRY_NOT_FOUND;
		}

		// The resource may be a leaf directly or a directory holding the
		// language leaves.
		const ResourceLeaf* currLeaf = firstLeaf(ResIter->child);
		if (!currLeaf)
		{
			return ERROR_ENTRY_NOT_FOUND;
		}

		data.assign(currLeaf->m_data.begin(), currLeaf->m_data.end());

		return ERROR_NONE;
	}

	/**
	* Sets the data of a resource, resource type and ID are specified by the parameters.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	* @param data The new data of the resource is taken from this vector.
	**/
	template<typename S, typename T>
	int ResourceDirectory::setResourceDataT(S restypeid, T resid, std::vector<byte>& data)
	{
		std::vector<ResourceChild>::iterator ResIter = locateResourceT(restypeid, resid);
		if (ResIter == m_rnRoot.children.end())
		{
			return ERROR_ENTRY_NOT_FOUND;
		}

		ResourceLeaf* currLeaf = firstLeaf(ResIter->child);
		if (!currLeaf)
		{
			return ERROR_ENTRY_NOT_FOUND;
		}

		currLeaf->m_data.assign(data.begin(), data.end());

		return ERROR_NONE;
	}

	/**
	* Returns the id of a resource, resource type and ID are specified by the parameters.
	* Note: Calling this function with resid == the ID of the resource makes no sense at all.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	* @return The ID of the specified resource.
	**/
	template<typename S, typename T>
	dword ResourceDirectory::getResourceIdT(S restypeid, T resid) const
	{
		std::vector<ResourceChild>::const_iterator ResIter = locateResourceT(restypeid, resid);
		return ResIter->entry.irde.Name;
	}

	/**
	* Sets the id of a resource, resource type and ID are specified by the parameters.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	* @param dwNewResId New ID of the resource.
	**/
	template<typename S, typename T>
	int ResourceDirectory::setResourceIdT(S restypeid, T resid, dword dwNewResId)
	{
		std::vector<ResourceChild>::iterator ResIter = locateResourceT(restypeid, resid);
		ResIter->entry.irde.Name = dwNewResId;
		return ERROR_NONE;
	}

	/**
	* Returns the name of a resource, resource type and ID are specified by the parameters.
	* Note: Calling this function with resid == the name of the resource makes no sense at all.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	* @return The name of the specified resource.
	**/
	template<typename S, typename T>
	std::string ResourceDirectory::getResourceNameT(S restypeid, T resid) const
	{
		std::vector<ResourceChild>::const_iterator ResIter = locateResourceT(restypeid, resid);
		return ResIter->entry.wstrName;
	}

	/**
	* Sets the name of a resource, resource type and ID are specified by the parameters.
	* @param restypeid Identifier of the resource type (either ID or name).
	* @param resid Identifier of the resource (either ID or name).
	* @param strNewResName The new name of the resource.
	**/
	template<typename S, typename T>
	int ResourceDirectory::setResourceNameT(S restypeid, T resid, std::string strNewResName)
	{
		std::vector<ResourceChild>::iterator ResIter = locateResourceT(restypeid, resid);
		ResIter->entry.wstrName = strNewResName;

		return ERROR_NONE;
	}

	template <int bits>
	class ResourceDirectoryT : public ResourceDirectory
	{
		public:
		  /// Reads the resource directory from a file.
		  int read(std::istream& inStream, const PeHeaderT<bits>& peHeader);
	};

	/**
	* Reads the resource directory from a file.
	*
	* Coordinates: all bytes of the resource structure are addressed by RVAs.
	* A directory header, an entry array, a UTF-16 name, and a data descriptor
	* are located by adding their directory-relative offset to the RVA of the
	* resource directory. A leaf's OffsetToData is an independent RVA and is
	* never interpreted relative to the resource directory. File offsets are
	* used only internally: each RVA is translated through the section mapping
	* that backs it, so a structure may span two virtually adjacent sections
	* whose raw data is not adjacent in the file. Sizes, CodePage, Reserved,
	* and getElementRva() stay raw values and getOccupiedAddresses() stays in
	* RVA space.
	*
	* Repeated reads: the same ResourceDirectory may read several images. The
	* tree, node offsets, occupied ranges, and loader error are reset first,
	* so after a successful read only the last image is visible.
	*
	* @param inStream Input stream.
	* @param peHeader A valid PE header which is necessary because some RVA
	* calculations need to be done.
	**/
	template <int bits>
	int ResourceDirectoryT<bits>::read(
			std::istream& inStream,
			const PeHeaderT<bits>& peHeader)
	{
		unsigned int uiResDirRva = peHeader.getIddResourceRva();

		resetReadState();
		m_resourceDirRva = uiResDirRva;
		m_sizeOfImage = peHeader.getSizeOfImage();

		// A zero RVA means the image has no resource directory.
		if (uiResDirRva == 0)
		{
			return ERROR_INVALID_FILE;
		}

		IStreamWrapper inStream_w(inStream);

		if (!inStream_w)
		{
			return ERROR_OPENING_FILE;
		}

		m_imageFileSize = fileSize(inStream_w);
		m_imageMappings = ResourceReader<bits>::buildImageMappings(peHeader, m_imageFileSize);

		std::uint64_t uiOffset = 0;
		if (!mapRvaToFileOffset(uiResDirRva, uiOffset)
			|| uiOffset >= m_imageFileSize
			|| uiResDirRva >= m_sizeOfImage)
		{
			return ERROR_INVALID_FILE;
		}

		m_readOffset = static_cast<unsigned int>(uiOffset);

		const int result = m_rnRoot.read(inStream_w, 0, this);
		if (result != ERROR_NONE)
		{
			// Do not leave a partially-read tree behind after a failed read.
			m_rnRoot.children.clear();
			m_resourceNodeOffsets.clear();
			m_occupiedAddresses.clear();
			m_readOffset = 0;
		}

		return result;
	}
}

#endif
